#include "android.h"

#include <android/api-level.h>
#include <android/log.h>
#include <qjnitypes.h>

#include <QtJniTypes>
#include <QByteArray>
#include <QMetaObject>
#include <QPointer>

#include <atomic>
#include <cstddef>
#include <unordered_map>

#include "controllers/midi/androidmidicontroller.h"

namespace mixxx {
namespace android {
std::mutex s_androidLock = {};
std::condition_variable s_grantingWaitCond = {};
std::vector<std::pair<QJniObject, bool>> s_grantingResult = {};
QJniObject s_intent = {};
QJniObject s_usbManager = {};
std::atomic<bool> s_usbPermissionGranted{false};

namespace {
std::mutex s_midiControllerLock;
std::unordered_map<int, QPointer<AndroidMidiController>> s_midiControllers;
std::atomic<int> s_nextMidiControllerId{1};

QPointer<AndroidMidiController> midiControllerForId(int controllerId) {
    std::lock_guard<std::mutex> lock(s_midiControllerLock);
    const auto it = s_midiControllers.find(controllerId);
    if (it == s_midiControllers.end()) {
        return {};
    }
    return it->second;
}
} // namespace

int registerMidiController(AndroidMidiController* controller) {
    if (!controller) {
        return -1;
    }
    const int id = s_nextMidiControllerId.fetch_add(1);
    std::lock_guard<std::mutex> lock(s_midiControllerLock);
    s_midiControllers[id] = QPointer<AndroidMidiController>(controller);
    return id;
}

void unregisterMidiController(int controllerId) {
    if (controllerId < 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_midiControllerLock);
    s_midiControllers.erase(controllerId);
}

const QJniObject& getIntent() {
    __android_log_print(ANDROID_LOG_VERBOSE, "mixxx", "about to get intent");
    std::unique_lock lock(s_androidLock);
    if (s_intent.isValid()) {
        return s_intent;
    }
    // QNativeInterface::QAndroidApplication::runOnAndroidMainThread([]() {
    if (!QNativeInterface::QAndroidApplication::isActivityContext()) {
        __android_log_print(ANDROID_LOG_WARN,
                "mixxx",
                "current context doesn't refer to an activity!");
    }

    QJniObject context = QNativeInterface::QAndroidApplication::context();

    s_usbManager = QJniObject("org/mixxx/UsbPermission");
    jint FLAG_MUTABLE =
            QJniObject::getStaticField<jint>(
                    "android/app/PendingIntent",
                    "FLAG_MUTABLE");
    QtJniTypes::String ACTION_USB_PERMISSION =
            QJniObject::fromString("org.mixxx.permissions.USB_PERMISSION");
    QtJniTypes::Intent intent = QJniObject("android/content/Intent",
            "(Ljava/lang/String;)V",
            ACTION_USB_PERMISSION.object<jstring>());
    if (!intent.isValid()) {
        __android_log_print(ANDROID_LOG_WARN, "mixxx", "pending intent is invalid!");
    }
    s_intent =
            QJniObject::callStaticMethod<jobject>("android/app/PendingIntent",
                    "getBroadcast",
                    "(Landroid/content/Context;ILandroid/content/"
                    "Intent;I)Landroid/app/PendingIntent;",
                    context,
                    0,
                    intent,
                    FLAG_MUTABLE);

    if (!s_intent.isValid()) {
        __android_log_print(ANDROID_LOG_WARN, "mixxx", "pending intent is invalid!");
    }

    __android_log_print(ANDROID_LOG_VERBOSE,
            "mixxx",
            "about to register the the receiver %d",
            s_usbManager.isValid());
    auto success = s_usbManager.callMethod<jboolean>("registerServiceBroadcastReceiver",
            "(Landroid/content/Context;)Z",
            context.object<jobject>());
    if (!success) {
        __android_log_print(ANDROID_LOG_WARN, "mixxx", "failed to registered the receiver!");
    }
    // });
    return s_intent;
}

bool waitForPermission(const QJniObject& device) {
    __android_log_print(ANDROID_LOG_VERBOSE, "mixxx", "about to wait for perm");
    std::unique_lock lock(s_androidLock);
    std::vector<std::pair<QJniObject, bool>>::const_iterator result = s_grantingResult.cend();

    int retries = 0;

    while (!s_grantingWaitCond.wait_for(
            lock, std::chrono::seconds(1), [&result, device] {
                result = std::find_if(s_grantingResult.cbegin(),
                        s_grantingResult.cend(),
                        [device](auto resultPair) {
                            return resultPair.first == device;
                        });
                return result != s_grantingResult.cend();
            })) {
        __android_log_print(ANDROID_LOG_VERBOSE,
                "mixxx",
                "Not found - current result count: %lu",
                s_grantingResult.size());
        QCoreApplication::processEvents();
        retries++;
        if (retries >= 10) {
            __android_log_print(ANDROID_LOG_WARN, "mixxx", "wait for perm timeout");
            qWarning() << "Timeout reached when waiting for Android permission to USB device";
            return false;
        }
    }
    __android_log_print(ANDROID_LOG_VERBOSE, "mixxx", "got perm result");
    return result->second;
}

void usbDeviceAccessResult(QJniObject device, bool granted) {
    std::unique_lock lock(s_androidLock);
    __android_log_print(ANDROID_LOG_WARN, "mixxx", "received permission result: %d", granted);
    s_grantingResult.push_back(std::make_pair<>(device, granted));
    s_grantingWaitCond.notify_one();
    // FIXME Handle large list?
}
} // namespace android
} // namespace mixxx

Q_DECLARE_JNI_CLASS(UsbPermissionClass, "org/mixxx/UsbPermission")
Q_DECLARE_JNI_CLASS(AndroidMidiHelperClass, "org/mixxx/AndroidMidiHelper")

void usbDeviceAccessResult(JNIEnv*, jobject, jobject device, jboolean granted) {
    mixxx::android::usbDeviceAccessResult(device, granted);
}
Q_DECLARE_JNI_NATIVE_METHOD(usbDeviceAccessResult)

// Native callback from AndroidMidiHelper.midiReceive()
static void midiReceive(JNIEnv* env,
        jobject,
        jint controllerId,
        jbyteArray data,
        jint offset,
        jint count,
        jlong timestamp) {
    Q_UNUSED(timestamp);

    if (!env || !data || offset < 0 || count <= 0) {
        return;
    }

    const jsize arrayLength = env->GetArrayLength(data);
    if (offset > arrayLength || count > arrayLength - offset) {
        return;
    }

    QByteArray bytes(count, '\0');
    env->GetByteArrayRegion(data,
            offset,
            count,
            reinterpret_cast<jbyte*>(bytes.data()));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return;
    }

    const auto controller = mixxx::android::midiControllerForId(controllerId);
    if (!controller) {
        return;
    }

    // Android's MidiReceiver callback may run on a Binder/handler thread.
    // Marshal processing to the controller's Qt thread and guard its lifetime.
    QMetaObject::invokeMethod(controller.data(),
            [controller, bytes = std::move(bytes)]() {
                if (controller) {
                    controller->receiveAndroidMidi(bytes);
                }
            },
            Qt::QueuedConnection);
}
Q_DECLARE_JNI_NATIVE_METHOD(midiReceive)

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) {
    QJniEnvironment env;
    env.registerNativeMethods<QtJniTypes::UsbPermissionClass>({
            Q_JNI_NATIVE_METHOD(usbDeviceAccessResult),
    });
    env.registerNativeMethods<QtJniTypes::AndroidMidiHelperClass>({
            Q_JNI_NATIVE_METHOD(midiReceive),
    });
    return JNI_VERSION_1_6;
}
