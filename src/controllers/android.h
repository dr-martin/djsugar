#pragma once

#include <QJniObject>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

struct libusb_context;
class AndroidMidiController;

namespace mixxx {
namespace android {

const QJniObject& getIntent();
bool waitForPermission(const QJniObject& device);
void usbDeviceAccessResult(QJniObject device, bool granted);

// Register Android MIDI controllers so Java MidiReceiver callbacks can be
// routed back to the correct native controller instance.
int registerMidiController(AndroidMidiController* controller);
void unregisterMidiController(int controllerId);

extern std::mutex s_androidLock;
extern std::condition_variable s_grantingWaitCond;
extern std::vector<std::pair<QJniObject, bool>> s_grantingResult;
extern QJniObject s_intent;
extern QJniObject s_usbManager;

/// True when a USB permission was recently granted (e.g. via USB_DEVICE_ATTACHED).
/// HID enumerator should re-scan when this is set.
extern std::atomic<bool> s_usbPermissionGranted;

} // namespace android
} // namespace mixxx
