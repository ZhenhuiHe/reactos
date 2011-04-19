/*
 * PROJECT:     ReactOS Universal Serial Bus Bulk Enhanced Host Controller Interface
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        drivers/usb/usbehci/hcd_controller.cpp
 * PURPOSE:     USB EHCI device driver.
 * PROGRAMMERS:
 *              Michael Martin (michael.martin@reactos.org)
 *              Johannes Anderwald (johannes.anderwald@reactos.org)
 */

#define INITGUID
#include "usbehci.h"
#include "hardware.h"

BOOLEAN
NTAPI
InterruptServiceRoutine(
    IN PKINTERRUPT  Interrupt,
    IN PVOID  ServiceContext);

VOID NTAPI
EhciDefferedRoutine(
    IN PKDPC Dpc,
    IN PVOID DeferredContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2);

class CUSBHardwareDevice : public IUSBHardwareDevice
{
public:
    STDMETHODIMP QueryInterface( REFIID InterfaceId, PVOID* Interface);

    STDMETHODIMP_(ULONG) AddRef()
    {
        InterlockedIncrement(&m_Ref);
        return m_Ref;
    }
    STDMETHODIMP_(ULONG) Release()
    {
        InterlockedDecrement(&m_Ref);

        if (!m_Ref)
        {
            delete this;
            return 0;
        }
        return m_Ref;
    }
    // com
    NTSTATUS Initialize(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT FunctionalDeviceObject, PDEVICE_OBJECT PhysicalDeviceObject, PDEVICE_OBJECT LowerDeviceObject);
    NTSTATUS PnpStart(PCM_RESOURCE_LIST RawResources, PCM_RESOURCE_LIST TranslatedResources);
    NTSTATUS PnpStop(void);
    NTSTATUS HandlePower(PIRP Irp);
    NTSTATUS GetDeviceDetails(PUSHORT VendorId, PUSHORT DeviceId, PULONG NumberOfPorts, PULONG Speed);
    NTSTATUS GetDMA(OUT struct IDMAMemoryManager **m_DmaManager);
    NTSTATUS GetUSBQueue(OUT struct IUSBQueue **OutUsbQueue);

    NTSTATUS StartController();
    NTSTATUS StopController();
    NTSTATUS ResetController();
    NTSTATUS ResetPort(ULONG PortIndex);

    NTSTATUS GetPortStatus(ULONG PortId, OUT USHORT *PortStatus, OUT USHORT *PortChange);
    NTSTATUS ClearPortStatus(ULONG PortId, ULONG Status);
    NTSTATUS SetPortFeature(ULONG PortId, ULONG Feature);

    VOID SetAsyncListRegister(ULONG PhysicalAddress);
    VOID SetPeriodicListRegister(ULONG PhysicalAddress);

    KIRQL AcquireDeviceLock(void);
    VOID ReleaseDeviceLock(KIRQL OldLevel);
    // local
    BOOLEAN InterruptService();

    // friend function
    friend BOOLEAN NTAPI InterruptServiceRoutine(IN PKINTERRUPT  Interrupt, IN PVOID  ServiceContext);
    friend VOID NTAPI EhciDefferedRoutine(IN PKDPC Dpc, IN PVOID DeferredContext, IN PVOID SystemArgument1, IN PVOID SystemArgument2);

    // constructor / destructor
    CUSBHardwareDevice(IUnknown *OuterUnknown){}
    virtual ~CUSBHardwareDevice(){}

protected:
    LONG m_Ref;
    PDRIVER_OBJECT m_DriverObject;
    PDEVICE_OBJECT m_PhysicalDeviceObject;
    PDEVICE_OBJECT m_FunctionalDeviceObject;
    PDEVICE_OBJECT m_NextDeviceObject;
    KSPIN_LOCK m_Lock;
    PKINTERRUPT m_Interrupt;
    KDPC m_IntDpcObject;
    PVOID VirtualBase;
    PHYSICAL_ADDRESS PhysicalAddress;
    PULONG m_Base;
    PDMA_ADAPTER m_Adapter;
    ULONG m_MapRegisters;
    EHCI_CAPS m_Capabilities;
    USHORT m_VendorID;
    USHORT m_DeviceID;
    PUSBQUEUE m_UsbQueue;
    PDMAMEMORYMANAGER m_MemoryManager;
    VOID SetCommandRegister(PEHCI_USBCMD_CONTENT UsbCmd);
    VOID GetCommandRegister(PEHCI_USBCMD_CONTENT UsbCmd);
    ULONG EHCI_READ_REGISTER_ULONG(ULONG Offset);
    VOID EHCI_WRITE_REGISTER_ULONG(ULONG Offset, ULONG Value);
};

//=================================================================================================
// COM
//
NTSTATUS
STDMETHODCALLTYPE
CUSBHardwareDevice::QueryInterface(
    IN  REFIID refiid,
    OUT PVOID* Output)
{
    if (IsEqualGUIDAligned(refiid, IID_IUnknown))
    {
        *Output = PVOID(PUNKNOWN(this));
        PUNKNOWN(*Output)->AddRef();
        return STATUS_SUCCESS;
    }

    return STATUS_UNSUCCESSFUL;
}

NTSTATUS
CUSBHardwareDevice::Initialize(
    PDRIVER_OBJECT DriverObject,
    PDEVICE_OBJECT FunctionalDeviceObject,
    PDEVICE_OBJECT PhysicalDeviceObject,
    PDEVICE_OBJECT LowerDeviceObject)
{
    BUS_INTERFACE_STANDARD BusInterface;
    PCI_COMMON_CONFIG PciConfig;
    NTSTATUS Status;
    ULONG BytesRead;

    DPRINT1("CUSBHardwareDevice::Initialize\n");

    //
    // Create DMAMemoryManager for use with QueueHeads and Transfer Descriptors.
    //
    Status =  CreateDMAMemoryManager(&m_MemoryManager);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to create DMAMemoryManager Object\n");
        return Status;
    }

    //
    // Create the UsbQueue class that will handle the Asynchronous and Periodic Schedules
    //
    Status = CreateUSBQueue(&m_UsbQueue);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to create UsbQueue!\n");
        return Status;
    }

    //
    // Initialize the DMAMemoryManager
    //
    Status = m_MemoryManager->Initialize(this, &m_Lock, PAGE_SIZE * 4, VirtualBase, PhysicalAddress, 32);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to initialize the DMAMemoryManager\n");
        return Status;
    }

    //
    // store device objects
    // 
    m_DriverObject = DriverObject;
    m_FunctionalDeviceObject = FunctionalDeviceObject;
    m_PhysicalDeviceObject = PhysicalDeviceObject;
    m_NextDeviceObject = LowerDeviceObject;

    //
    // initialize device lock
    //
    KeInitializeSpinLock(&m_Lock);

    m_VendorID = 0;
    m_DeviceID = 0;

    Status = GetBusInterface(PhysicalDeviceObject, &BusInterface);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to get BusInteface!\n");
        return Status;
    }

    BytesRead = (*BusInterface.GetBusData)(BusInterface.Context,
                                           PCI_WHICHSPACE_CONFIG,
                                           &PciConfig,
                                           0,
                                           PCI_COMMON_HDR_LENGTH);

    if (BytesRead != PCI_COMMON_HDR_LENGTH)
    {
        DPRINT1("Failed to get pci config information!\n");
        return STATUS_SUCCESS;
    }

    if (!(PciConfig.Command & PCI_ENABLE_BUS_MASTER))
    {
        DPRINT1("PCI Configuration shows this as a non Bus Mastering device!\n");
    }

    m_VendorID = PciConfig.VendorID;
    m_DeviceID = PciConfig.DeviceID;

    return STATUS_SUCCESS;
}

VOID
CUSBHardwareDevice::SetCommandRegister(PEHCI_USBCMD_CONTENT UsbCmd)
{
    PULONG Register;
    Register = (PULONG)UsbCmd;
    WRITE_REGISTER_ULONG((PULONG)((ULONG)m_Base + EHCI_USBCMD), *Register);
}

VOID
CUSBHardwareDevice::GetCommandRegister(PEHCI_USBCMD_CONTENT UsbCmd)
{
    PULONG Register;
    Register = (PULONG)UsbCmd;
    *Register = READ_REGISTER_ULONG((PULONG)((ULONG)m_Base + EHCI_USBCMD));
}

ULONG
CUSBHardwareDevice::EHCI_READ_REGISTER_ULONG(ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)((ULONG)m_Base + Offset));
}

VOID
CUSBHardwareDevice::EHCI_WRITE_REGISTER_ULONG(ULONG Offset, ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)((ULONG)m_Base + Offset), Value);
}

NTSTATUS
CUSBHardwareDevice::PnpStart(
    PCM_RESOURCE_LIST RawResources,
    PCM_RESOURCE_LIST TranslatedResources)
{
    ULONG Index, Count;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR ResourceDescriptor;
    DEVICE_DESCRIPTION DeviceDescription;
    PVOID ResourceBase;
    NTSTATUS Status;

    DPRINT1("CUSBHardwareDevice::PnpStart\n");
    for(Index = 0; Index < TranslatedResources->List[0].PartialResourceList.Count; Index++)
    {
        //
        // get resource descriptor
        //
        ResourceDescriptor = &TranslatedResources->List[0].PartialResourceList.PartialDescriptors[Index];

        switch(ResourceDescriptor->Type)
        {
            case CmResourceTypeInterrupt:
            {
                KeInitializeDpc(&m_IntDpcObject,
                                EhciDefferedRoutine,
                                this);

                Status = IoConnectInterrupt(&m_Interrupt,
                                            InterruptServiceRoutine,
                                            (PVOID)this,
                                            NULL,
                                            ResourceDescriptor->u.Interrupt.Vector,
                                            (KIRQL)ResourceDescriptor->u.Interrupt.Level,
                                            (KIRQL)ResourceDescriptor->u.Interrupt.Level,
                                            (KINTERRUPT_MODE)(ResourceDescriptor->Flags & CM_RESOURCE_INTERRUPT_LATCHED),
                                            (ResourceDescriptor->ShareDisposition != CmResourceShareDeviceExclusive),
                                            ResourceDescriptor->u.Interrupt.Affinity,
                                            FALSE);

                if (!NT_SUCCESS(Status))
                {
                    //
                    // failed to register interrupt
                    //
                    DPRINT1("IoConnect Interrupt failed with %x\n", Status);
                    return Status;
                }
                break;
            }
            case CmResourceTypeMemory:
            {
                //
                // get resource base
                //
                ResourceBase = MmMapIoSpace(ResourceDescriptor->u.Memory.Start, ResourceDescriptor->u.Memory.Length, MmNonCached);
                if (!ResourceBase)
                {
                    //
                    // failed to map registers
                    //
                    DPRINT1("MmMapIoSpace failed\n");
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                //
                // Get controllers capabilities 
                //
                m_Capabilities.Length = READ_REGISTER_UCHAR((PUCHAR)ResourceBase);
                m_Capabilities.HCIVersion = READ_REGISTER_USHORT((PUSHORT)((ULONG)ResourceBase + 2));
                m_Capabilities.HCSParamsLong = READ_REGISTER_ULONG((PULONG)((ULONG)ResourceBase + 4));
                m_Capabilities.HCCParamsLong = READ_REGISTER_ULONG((PULONG)((ULONG)ResourceBase + 8));

                DPRINT1("Controller has %d Ports\n", m_Capabilities.HCSParams.PortCount);
                if (m_Capabilities.HCSParams.PortRouteRules)
                {
                    for (Count = 0; Count < m_Capabilities.HCSParams.PortCount; Count++)
                    {
                        m_Capabilities.PortRoute[Count] = READ_REGISTER_UCHAR((PUCHAR)(ULONG)ResourceBase + 12 + Count);
                    }
                }

                //
                // Set m_Base to the address of Operational Register Space
                //
                m_Base = (PULONG)((ULONG)ResourceBase + m_Capabilities.Length);
                break;
            }
        }
    }


    //
    // zero device description
    //
    RtlZeroMemory(&DeviceDescription, sizeof(DEVICE_DESCRIPTION));

    //
    // initialize device description
    //
    DeviceDescription.Version = DEVICE_DESCRIPTION_VERSION;
    DeviceDescription.Master = TRUE;
    DeviceDescription.ScatterGather = TRUE;
    DeviceDescription.Dma32BitAddresses = TRUE;
    DeviceDescription.DmaWidth = Width32Bits;
    DeviceDescription.InterfaceType = PCIBus;
    DeviceDescription.MaximumLength = MAXULONG;

    //
    // get dma adapter
    //
    m_Adapter = IoGetDmaAdapter(m_PhysicalDeviceObject, &DeviceDescription, &m_MapRegisters);
    if (!m_Adapter)
    {
        //
        // failed to get dma adapter
        //
        DPRINT1("Failed to acquire dma adapter\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // Create Common Buffer
    //
    VirtualBase = m_Adapter->DmaOperations->AllocateCommonBuffer(m_Adapter,
                                                                 PAGE_SIZE * 4,
                                                                 &PhysicalAddress,
                                                                 FALSE);
    if (!VirtualBase)
    {
        DPRINT1("Failed to allocate a common buffer\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // Stop the controller before modifying schedules
    //
    Status = StopController();
    if (!NT_SUCCESS(Status))
        return Status;

    //
    // Initialize the UsbQueue now that we have an AdapterObject.
    //
    Status = m_UsbQueue->Initialize(PUSBHARDWAREDEVICE(this), m_Adapter, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to Initialize the UsbQueue\n");
        return Status;
    }

    //
    // Start the controller
    //

    DPRINT1("Starting Controller\n");
    return StartController();
}

NTSTATUS
CUSBHardwareDevice::PnpStop(void)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
CUSBHardwareDevice::HandlePower(
    PIRP Irp)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
CUSBHardwareDevice::GetDeviceDetails(
    OUT OPTIONAL PUSHORT VendorId,
    OUT OPTIONAL PUSHORT DeviceId,
    OUT OPTIONAL PULONG NumberOfPorts,
    OUT OPTIONAL PULONG Speed)
{
    if (VendorId)
        *VendorId = m_VendorID;
    if (DeviceId)
        *DeviceId = m_DeviceID;
    if (NumberOfPorts)
        *NumberOfPorts = m_Capabilities.HCSParams.PortCount;
    //FIXME: What to returned here?
    if (Speed)
        *Speed = 0x200;
    return STATUS_SUCCESS;
}

NTSTATUS CUSBHardwareDevice::GetDMA(
    OUT struct IDMAMemoryManager **OutDMAMemoryManager)
{
    if (!m_MemoryManager)
        return STATUS_UNSUCCESSFUL;
    *OutDMAMemoryManager = m_MemoryManager;
    return STATUS_SUCCESS;
}

NTSTATUS
CUSBHardwareDevice::GetUSBQueue(
    OUT struct IUSBQueue **OutUsbQueue)
{
    if (!m_UsbQueue)
        return STATUS_UNSUCCESSFUL;
    *OutUsbQueue = m_UsbQueue;
    return STATUS_SUCCESS;
}


NTSTATUS
CUSBHardwareDevice::StartController(void)
{
    EHCI_USBCMD_CONTENT UsbCmd;
    ULONG UsbSts, FailSafe;

    //
    // Stop the controller if its running
    //
    UsbSts = EHCI_READ_REGISTER_ULONG(EHCI_USBSTS);
    if (!(UsbSts & EHCI_STS_HALT))
        StopController();

    //
    // Reset the device. Bit is set to 0 on completion.
    //
    GetCommandRegister(&UsbCmd);
    UsbCmd.HCReset = TRUE;
    SetCommandRegister(&UsbCmd);

    //
    // Check that the controller reset
    //
    for (FailSafe = 100; FailSafe > 1; FailSafe--)
    {
        KeStallExecutionProcessor(10);
        GetCommandRegister(&UsbCmd);
        if (!UsbCmd.HCReset)
        {
            break;
        }
    }

    //
    // If the controller did not reset then fail
    //
    if (UsbCmd.HCReset)
    {
        DPRINT1("EHCI ERROR: Controller failed to reset. Hardware problem!\n");
        return STATUS_UNSUCCESSFUL;
    }

    //
    // Disable Interrupts and clear status
    //
    EHCI_WRITE_REGISTER_ULONG(EHCI_USBINTR, 0);
    EHCI_WRITE_REGISTER_ULONG(EHCI_USBSTS, 0x0000001f);

    //
    // FIXME: Assign the AsyncList Register
    //

    //
    // Set Schedules to Enable and Interrupt Threshold to 1ms.
    //
    GetCommandRegister(&UsbCmd);
    UsbCmd.PeriodicEnable = FALSE;
    UsbCmd.AsyncEnable = FALSE;  //FIXME: Need USB Memory Manager

    UsbCmd.IntThreshold = 1;
    // FIXME: Set framelistsize when periodic is implemented.
    SetCommandRegister(&UsbCmd);

    //
    // Enable Interrupts and start execution
    //
    EHCI_WRITE_REGISTER_ULONG(EHCI_USBINTR, EHCI_USBINTR_INTE | EHCI_USBINTR_ERR | EHCI_USBINTR_ASYNC | EHCI_USBINTR_HSERR
        /*| EHCI_USBINTR_FLROVR*/  | EHCI_USBINTR_PC);

    UsbCmd.Run = TRUE;
    SetCommandRegister(&UsbCmd);

    //
    // Wait for execution to start
    //
    for (FailSafe = 100; FailSafe > 1; FailSafe--)
    {
        KeStallExecutionProcessor(10);
        UsbSts = EHCI_READ_REGISTER_ULONG(EHCI_USBSTS);

        if (!(UsbSts & EHCI_STS_HALT))
        {
            break;
        }
    }

    if (UsbSts & EHCI_STS_HALT)
    {
        DPRINT1("Could not start execution on the controller\n");
        return STATUS_UNSUCCESSFUL;
    }

    //
    // Set port routing to EHCI controller
    //
    EHCI_WRITE_REGISTER_ULONG(EHCI_CONFIGFLAG, 1);

    DPRINT1("EHCI Started!\n");
    return STATUS_SUCCESS;
}

NTSTATUS
CUSBHardwareDevice::StopController(void)
{
    EHCI_USBCMD_CONTENT UsbCmd;
    ULONG UsbSts, FailSafe;

    //
    // Disable Interrupts and stop execution
    //
    EHCI_WRITE_REGISTER_ULONG (EHCI_USBINTR, 0);

    GetCommandRegister(&UsbCmd);
    UsbCmd.Run = FALSE;
    SetCommandRegister(&UsbCmd);

    for (FailSafe = 100; FailSafe > 1; FailSafe--)
    {
        KeStallExecutionProcessor(10);
        UsbSts = EHCI_READ_REGISTER_ULONG(EHCI_USBSTS);
        if (UsbSts & EHCI_STS_HALT)
        {
            break;
        }
    }

    if (!(UsbSts & EHCI_STS_HALT))
    {
        DPRINT1("EHCI ERROR: Controller is not responding to Stop request!\n");
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
CUSBHardwareDevice::ResetController(void)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
CUSBHardwareDevice::ResetPort(
    IN ULONG PortIndex)
{
    ULONG PortStatus;

    if (PortIndex > m_Capabilities.HCSParams.PortCount)
        return STATUS_UNSUCCESSFUL;

    PortStatus = EHCI_READ_REGISTER_ULONG(EHCI_PORTSC + (4 * PortIndex));
    if (PortStatus & EHCI_PRT_SLOWSPEEDLINE)
    {
        DPRINT1("Non HighSpeed device. Releasing Ownership\n");
        EHCI_WRITE_REGISTER_ULONG(EHCI_PORTSC + (4 * PortIndex), EHCI_PRT_RELEASEOWNERSHIP);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    //
    // Reset and clean enable
    //
    PortStatus |= EHCI_PRT_RESET;
    PortStatus &= ~EHCI_PRT_ENABLED;
    EHCI_WRITE_REGISTER_ULONG(EHCI_PORTSC + (4 * PortIndex), PortStatus);

    KeStallExecutionProcessor(100);

    //
    // Clear reset
    //
    PortStatus = EHCI_READ_REGISTER_ULONG(EHCI_PORTSC + (4 * PortIndex));
    PortStatus &= ~EHCI_PRT_RESET;
    EHCI_WRITE_REGISTER_ULONG(EHCI_PORTSC + (4 * PortIndex), PortStatus);

    KeStallExecutionProcessor(100);

    //
    // Check that the port reset
    //
    PortStatus = EHCI_READ_REGISTER_ULONG(EHCI_PORTSC + (4 * PortIndex));
    if (PortStatus & EHCI_PRT_RESET)
    {
        DPRINT1("Port did not reset\n");
        return STATUS_RETRY;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
CUSBHardwareDevice::GetPortStatus(
    ULONG PortId,
    OUT USHORT *PortStatus,
    OUT USHORT *PortChange)
{
    ULONG Value;
    USHORT Status = 0, Change = 0;

    DPRINT1("CUSBHardwareDevice::GetPortStatus\n");

    if (PortId > m_Capabilities.HCSParams.PortCount)
        return STATUS_UNSUCCESSFUL;
    
    //
    // Get the value of the Port Status and Control Register
    //
    Value = EHCI_READ_REGISTER_ULONG(EHCI_PORTSC + (4 * PortId));

    //
    // If the PowerPortControl is 0 then host controller does not have power control switches
    if (!m_Capabilities.HCSParams.PortPowerControl)
    {
        Status |= USB_PORT_STATUS_POWER;
    }
    else
    {
        // Check the value of PortPower
        if (Value & EHCI_PRT_POWER)
        {
            Status |= USB_PORT_STATUS_POWER;
        }
    }

    // Get Speed. If SlowSpeedLine flag is there then its a slow speed device
    if (Value & EHCI_PRT_SLOWSPEEDLINE)
        Status |= USB_PORT_STATUS_LOW_SPEED;
    else
        Status |= USB_PORT_STATUS_HIGH_SPEED;

    // Get Connected Status
    if (Value & EHCI_PRT_CONNECTED)
        Status |= USB_PORT_STATUS_CONNECT;

    // Get Enabled Status
    if (Value & EHCI_PRT_ENABLED)
        Status |= USB_PORT_STATUS_ENABLE;

    // Is it suspended?
    if (Value & EHCI_PRT_SUSPEND)
        Status |= USB_PORT_STATUS_SUSPEND;

    // a overcurrent is active?
    if (Value & EHCI_PRT_OVERCURRENTACTIVE)
        Status |= USB_PORT_STATUS_OVER_CURRENT;

    // In a reset state?
    if (Value & EHCI_PRT_RESET)
        Status |= USB_PORT_STATUS_RESET;

    //
    // FIXME: Is the Change here correct?
    //
    if (Value & EHCI_PRT_CONNECTSTATUSCHANGE)
        Change |= USB_PORT_STATUS_CONNECT;

    if (Value & EHCI_PRT_ENABLEDSTATUSCHANGE)
        Change |= USB_PORT_STATUS_ENABLE;

    *PortStatus = Status;
    *PortChange = Change;

    //HACK: Maybe
    if (Status == (USB_PORT_STATUS_HIGH_SPEED | USB_PORT_STATUS_CONNECT | USB_PORT_STATUS_POWER))
        *PortChange = USB_PORT_STATUS_CONNECT;
    
    return STATUS_SUCCESS;
}

NTSTATUS
CUSBHardwareDevice::ClearPortStatus(
    ULONG PortId,
    ULONG Status)
{
    ULONG Value;

    DPRINT1("CUSBHardwareDevice::ClearPortStatus\n");

    if (PortId > m_Capabilities.HCSParams.PortCount)
        return STATUS_UNSUCCESSFUL;

    Value = EHCI_READ_REGISTER_ULONG(EHCI_PORTSC + (4 * PortId));

    if (Status == C_PORT_RESET)
    {
        if (Value & EHCI_PRT_RESET)
        {
            Value &= ~EHCI_PRT_RESET;
            EHCI_WRITE_REGISTER_ULONG(EHCI_PORTSC + (4 * PortId), Value);
            KeStallExecutionProcessor(100);
        }
    }

    if (Status == C_PORT_CONNECTION)
    {
        // FIXME: Make sure its the Connection and Enable Change status.
        Value |= EHCI_PRT_CONNECTSTATUSCHANGE;
        Value |= EHCI_PRT_ENABLEDSTATUSCHANGE;
        EHCI_WRITE_REGISTER_ULONG(EHCI_PORTSC + (4 * PortId), Value);
    }

    return STATUS_SUCCESS;
}


NTSTATUS
CUSBHardwareDevice::SetPortFeature(
    ULONG PortId,
    ULONG Feature)
{
    ULONG Value;

    DPRINT1("CUSBHardwareDevice::SetPortFeature\n");

    if (PortId > m_Capabilities.HCSParams.PortCount)
        return STATUS_UNSUCCESSFUL;

    Value = EHCI_READ_REGISTER_ULONG(EHCI_PORTSC + (4 * PortId));

    if (Feature == PORT_ENABLE)
    {
        //
        // FIXME: EHCI Ports can only be disabled via reset
        //
        DPRINT1("PORT_ENABLE not supported for EHCI\n");
    }
    
    if (Feature == PORT_RESET)
    {
        if (Value & EHCI_PRT_SLOWSPEEDLINE)
        {
            DPRINT1("Non HighSpeed device. Releasing Ownership\n");
        }
        //
        // Reset and clean enable
        //
        Value |= EHCI_PRT_RESET;
        Value &= ~EHCI_PRT_ENABLED;
        EHCI_WRITE_REGISTER_ULONG(EHCI_PORTSC + (4 * PortId), Value);

        KeStallExecutionProcessor(100);
    }
    
    if (Feature == PORT_POWER)
        DPRINT1("PORT_POWER Not implemented\n");

    return STATUS_SUCCESS;
}

VOID
CUSBHardwareDevice::SetAsyncListRegister(
    ULONG PhysicalAddress)
{
    EHCI_WRITE_REGISTER_ULONG(EHCI_ASYNCLISTBASE, PhysicalAddress);
}

VOID
CUSBHardwareDevice::SetPeriodicListRegister(
    ULONG PhysicalAddress)
{
    EHCI_WRITE_REGISTER_ULONG(EHCI_PERIODICLISTBASE, PhysicalAddress);
}

KIRQL
CUSBHardwareDevice::AcquireDeviceLock(void)
{
    KIRQL OldLevel;

    //
    // acquire lock
    //
    KeAcquireSpinLock(&m_Lock, &OldLevel);

    //
    // return old irql
    //
    return OldLevel;
}


VOID
CUSBHardwareDevice::ReleaseDeviceLock(
    KIRQL OldLevel)
{
    KeReleaseSpinLock(&m_Lock, OldLevel);
}

BOOLEAN
NTAPI
InterruptServiceRoutine(
    IN PKINTERRUPT  Interrupt,
    IN PVOID  ServiceContext)
{
    CUSBHardwareDevice *This;
    ULONG CStatus;

    This = (CUSBHardwareDevice*) ServiceContext;
    CStatus = This->EHCI_READ_REGISTER_ULONG(EHCI_USBSTS);

    CStatus &= (EHCI_ERROR_INT | EHCI_STS_INT | EHCI_STS_IAA | EHCI_STS_PCD | EHCI_STS_FLR);
    //
    // Check that it belongs to EHCI
    //
    if (!CStatus)
        return FALSE;

    //
    // Clear the Status
    //
    This->EHCI_WRITE_REGISTER_ULONG(EHCI_USBSTS, CStatus);

    if (CStatus & EHCI_STS_FATAL)
    {
        This->StopController();
        DPRINT1("EHCI: Host System Error!\n");
        return TRUE;
    }

    if (CStatus & EHCI_ERROR_INT)
    {
        DPRINT1("EHCI Status = 0x%x\n", CStatus);
    }

    if (CStatus & EHCI_STS_HALT)
    {
        DPRINT1("Host Error Unexpected Halt\n");
        // FIXME: Reset controller\n");
        return TRUE;
    }

    KeInsertQueueDpc(&This->m_IntDpcObject, This, (PVOID)CStatus);
    return TRUE;
}

VOID NTAPI
EhciDefferedRoutine(
    IN PKDPC Dpc,
    IN PVOID DeferredContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2)
{
    CUSBHardwareDevice *This;
    ULONG CStatus, PortStatus, PortCount, i;

    This = (CUSBHardwareDevice*) SystemArgument1;
    CStatus = (ULONG) SystemArgument2;

    This->GetDeviceDetails(NULL, NULL, &PortCount, NULL);
    if (CStatus & EHCI_STS_PCD)
    {
        for (i = 0; i < PortCount; i++)
        {
            PortStatus = This->EHCI_READ_REGISTER_ULONG(EHCI_PORTSC + (4 * i));

            //
            // Device connected or removed
            //
            if (PortStatus & EHCI_PRT_CONNECTSTATUSCHANGE)
            {
                //
                // Clear the port change status
                //
                This->EHCI_WRITE_REGISTER_ULONG(EHCI_PORTSC + (4 * i), PortStatus | EHCI_PRT_CONNECTSTATUSCHANGE);

                if (PortStatus & EHCI_PRT_CONNECTED)
                {
                    DPRINT1("Device connected on port %d\n", i);

                    //
                    //FIXME: Determine device speed
                    //
                    if (This->m_Capabilities.HCSParams.CHCCount)
                    {
                        if (PortStatus & EHCI_PRT_ENABLED)
                        {
                            DPRINT1("Misbeaving controller. Port should be disabled at this point\n");
                        }

                        if (PortStatus & EHCI_PRT_SLOWSPEEDLINE)
                        {
                            DPRINT1("Non HighSeped device connected. Release ownership\n");
                            This->EHCI_WRITE_REGISTER_ULONG(EHCI_PORTSC + (4 * i), EHCI_PRT_RELEASEOWNERSHIP);
                            continue;
                        }

                        //
                        // FIXME: Is a port reset needed, or does hub driver request this?
                        //
                    }
                }
                else
                {
                    DPRINT1("Device disconnected on port %d\n", i);
                }

                //
                // FIXME: This needs to be saved somewhere
                //
            }
        }
    }
    return;
}

NTSTATUS
CreateUSBHardware(
    PUSBHARDWAREDEVICE *OutHardware)
{
    PUSBHARDWAREDEVICE This;

    This = new(NonPagedPool, TAG_USBEHCI) CUSBHardwareDevice(0);

    if (!This)
        return STATUS_INSUFFICIENT_RESOURCES;

    This->AddRef();

    // return result
    *OutHardware = (PUSBHARDWAREDEVICE)This;

    return STATUS_SUCCESS;
}
