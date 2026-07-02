#include "usb_link.h"

namespace stepbridge
{
UsbLink *UsbLink::sInstance = nullptr;
}

// TinyUSB device-stack callbacks. This board has no VBUS-detect circuit, so
// per EvoSeq's confirmed finding, a real disconnect/reconnect is at least as
// likely to surface as tud_resume_cb (suspend->resume) as a fresh
// tud_mount_cb - both reset the incoming-SysEx parser state, since a
// disconnect mid-message would otherwise leave sysexActive_ stuck true
// forever.
void tud_mount_cb()
{
	if (stepbridge::UsbLink::sInstance) stepbridge::UsbLink::sInstance->OnUsbReconnect();
}

void tud_resume_cb()
{
	if (stepbridge::UsbLink::sInstance) stepbridge::UsbLink::sInstance->OnUsbReconnect();
}
