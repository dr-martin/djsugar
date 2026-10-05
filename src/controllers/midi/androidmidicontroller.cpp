#ifdef __ANDROID__

#include "controllers/midi/androidmidicontroller.h"

#include <android/log.h>
#include <unistd.h>

#include <QJniEnvironment>
#if __has_include(<QNativeInterface/QAndroidApplication>)
#include <QNativeInterface/QAndroidApplication>
#endif

#include "controllers/android.h"
#include "controllers/defs_controllers.h"
#include "controllers/midi/midiutils.h"
#include "moc_androidmidicontroller.cpp"

namespace {
const mixxx::Logger kLogger("AndroidMidiController");
constexpr int kUsbMidiPacketSize = 64;
constexpr int kPollTimeoutMs = 10;
constexpr int kMidiMsgSize = 3;
} // namespace

AndroidMidiController::AndroidMidiController(const QString& name,
        const QJniObject& midiDeviceInfo,
        const QJniObject& usbDevice,
        int interfaceNumber,
        int inputPortIndex,
        int outputPortIndex,
        uint16_t vendorId,
        uint16_t productId,
        const QString& vendorStr,
        const QString& productStr)
        : MidiController(name),
          m_midiDeviceInfo(midiDeviceInfo),
          m_usbDevice(usbDevice),
          m_interfaceNumber(interfaceNumber),
          m_inputPortIndex(inputPortIndex),
          m_outputPortIndex(outputPortIndex),
          m_vendorId(vendorId),
          m_productId(productId),
          m_vendor(vendorStr),
          m_product(productStr) {
}

AndroidMidiController::~AndroidMidiController() {
    close();
}

int AndroidMidiController::open(const QString& resourcePath) {
    if (isOpen()) {
        return 0;
    }

    // Prefer Android's native MIDI API whenever the enumerator supplied a
    // MidiDeviceInfo. This is the same API used by Android MIDI monitor apps
    // and avoids competing with Android for the USB MIDI interface.
    const int result = m_midiDeviceInfo.isValid()
            ? openWithMidiManager()
            : openWithUsbBulk();
    if (result != 0) {
        return result;
    }

    // Keep the Android implementation consistent with every other Mixxx
    // controller backend: an open transport must also have a running mapping
    // engine and must advertise itself as open. Without this, reconnecting the
    // USB audio side can leave MIDI callbacks alive but the controller lifecycle
    // in a half-open state.
    startEngine();
    if (!applyMapping(resourcePath)) {
        kLogger.warning() << "Could not apply Android MIDI mapping for" << getName();
        close();
        return 1;
    }
    setOpen(true);
    return 0;
}

int AndroidMidiController::openWithMidiManager() {
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid()) {
        kLogger.warning() << "No Android context";
        return 1;
    }

    QJniObject MIDI_SERVICE =
            QJniObject::getStaticObjectField("android/content/Context",
                    "MIDI_SERVICE",
                    "Ljava/lang/String;");
    auto midiManager = context.callObjectMethod("getSystemService",
            "(Ljava/lang/String;)Ljava/lang/Object;",
            MIDI_SERVICE.object());
    if (!midiManager.isValid()) {
        kLogger.warning() << "Cannot get Android MidiManager";
        return 1;
    }

    // Audio reconfiguration of a composite USB DJ controller may cause Android
    // to refresh its logical MIDI device. Re-resolve MidiDeviceInfo before every
    // open so a reconnect does not keep using a stale Java object.
    const jint previousDeviceId = m_midiDeviceInfo.isValid()
            ? m_midiDeviceInfo.callMethod<jint>("getId")
            : -1;
    QJniObject currentDeviceInfo;
    QJniObject matchingNameDeviceInfo;
    auto deviceInfoArray = midiManager.callObjectMethod(
            "getDevices", "()[Landroid/media/midi/MidiDeviceInfo;");
    if (deviceInfoArray.isValid()) {
        QJniEnvironment env;
        const jsize count = env->GetArrayLength(
                static_cast<jobjectArray>(deviceInfoArray.object()));
        const QJniObject nameKey = QJniObject::fromString("name");
        for (jsize i = 0; i < count; ++i) {
            QJniObject candidate = env->GetObjectArrayElement(
                    static_cast<jobjectArray>(deviceInfoArray.object()), i);
            if (!candidate.isValid()) {
                continue;
            }
            if (candidate.callMethod<jint>("getId") == previousDeviceId) {
                currentDeviceInfo = candidate;
                break;
            }
            QJniObject props = candidate.callObjectMethod(
                    "getProperties", "()Landroid/os/Bundle;");
            if (!props.isValid()) {
                continue;
            }
            QJniObject name = props.callObjectMethod(
                    "getString",
                    "(Ljava/lang/String;)Ljava/lang/String;",
                    nameKey.object());
            if (name.isValid() && name.toString() == getName()) {
                matchingNameDeviceInfo = candidate;
            }
        }
    }
    if (!currentDeviceInfo.isValid()) {
        currentDeviceInfo = matchingNameDeviceInfo;
    }
    if (currentDeviceInfo.isValid()) {
        m_midiDeviceInfo = currentDeviceInfo;
    }

    m_midiHelper = QJniObject("org/mixxx/AndroidMidiHelper", "()V");
    if (!m_midiHelper.isValid()) {
        kLogger.warning() << "Cannot create AndroidMidiHelper";
        return 1;
    }

    m_controllerId = mixxx::android::registerMidiController(this);
    if (m_controllerId < 0) {
        kLogger.warning() << "Cannot register Android MIDI controller";
        m_midiHelper = QJniObject();
        return 1;
    }

    const bool opened = m_midiHelper.callMethod<jboolean>("open",
            "(Landroid/media/midi/MidiManager;"
            "Landroid/media/midi/MidiDeviceInfo;I)Z",
            midiManager.object(),
            m_midiDeviceInfo.object(),
            static_cast<jint>(m_controllerId));
    if (!opened) {
        kLogger.warning() << "Android MidiManager could not open" << getName();
        mixxx::android::unregisterMidiController(m_controllerId);
        m_controllerId = -1;
        m_midiHelper = QJniObject();
        return 1;
    }

    const bool portsOpened = m_midiHelper.callMethod<jboolean>("openPorts",
            "(II)Z",
            static_cast<jint>(m_inputPortIndex),
            static_cast<jint>(m_outputPortIndex));
    if (!portsOpened) {
        kLogger.warning() << "Android MIDI ports could not be opened for" << getName();
        m_midiHelper.callMethod<void>("close");
        mixxx::android::unregisterMidiController(m_controllerId);
        m_controllerId = -1;
        m_midiHelper = QJniObject();
        return 1;
    }

    m_usingMidiManager = true;
    kLogger.info() << "Android MIDI controller opened through MidiManager:"
                   << getName()
                   << "input port:" << m_inputPortIndex
                   << "output port:" << m_outputPortIndex;
    return 0;
}

int AndroidMidiController::openWithUsbBulk() {
    if (m_pIoThread && m_pIoThread->isRunning()) {
        return 0;
    }

    if (!m_usbDevice.isValid() || m_interfaceNumber < 0) {
        kLogger.warning() << "No usable USB MIDI device/interface";
        return 1;
    }

    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid()) {
        kLogger.warning() << "No Android context";
        return 1;
    }

    QJniObject USB_SERVICE =
            QJniObject::getStaticObjectField("android/content/Context",
                    "USB_SERVICE",
                    "Ljava/lang/String;");
    auto usbManager = context.callObjectMethod("getSystemService",
            "(Ljava/lang/String;)Ljava/lang/Object;",
            USB_SERVICE.object());

    if (!usbManager.isValid()) {
        kLogger.warning() << "No UsbManager";
        return 1;
    }

    // Request USB permission for the raw-USB fallback path.
    if (!usbManager.callMethod<jboolean>("hasPermission",
                "(Landroid/hardware/usb/UsbDevice;)Z",
                m_usbDevice.object())) {
        kLogger.info() << "Requesting USB permission";
        auto pendingIntent = mixxx::android::getIntent();
        usbManager.callMethod<void>("requestPermission",
                "(Landroid/hardware/usb/UsbDevice;Landroid/app/PendingIntent;)V",
                m_usbDevice.object(),
                pendingIntent.object());
        if (!mixxx::android::waitForPermission(m_usbDevice)) {
            kLogger.warning() << "USB permission denied";
            return 1;
        }
    }

    // The fallback I/O thread owns the connection used for transfer. Do not
    // claim the same interface here and then reopen it a second time.
    auto usbConnection = usbManager.callObjectMethod("openDevice",
            "(Landroid/hardware/usb/UsbDevice;)Landroid/hardware/usb/UsbDeviceConnection;",
            m_usbDevice.object());
    if (!usbConnection.isValid()) {
        kLogger.warning() << "Cannot open USB device";
        return 1;
    }

    const jint usbFd = usbConnection.callMethod<jint>("getFileDescriptor");
    if (usbFd < 0) {
        kLogger.warning() << "No file descriptor";
        return 1;
    }

    m_pIoThread = new IoThread(m_usbDevice,
            usbFd,
            m_interfaceNumber,
            this);
    m_pIoThread->start();

    kLogger.info() << "Android MIDI controller opened via raw USB fallback:"
                   << getProductString() << "USB FD:" << usbFd;
    return 0;
}

int AndroidMidiController::close() {
    // Keep the transport available while the controller engine shuts down, in
    // case a mapping sends final LED/state messages.
    if (getScriptEngine()) {
        stopEngine();
    }
    const int baseResult = MidiController::close();

    if (m_usingMidiManager || m_midiHelper.isValid()) {
        if (m_midiHelper.isValid()) {
            m_midiHelper.callMethod<void>("close");
        }
        mixxx::android::unregisterMidiController(m_controllerId);
        m_controllerId = -1;
        m_usingMidiManager = false;
        m_midiHelper = QJniObject();
    }

    if (m_pIoThread) {
        m_pIoThread->stop();
        if (!m_pIoThread->wait(2000)) {
            kLogger.warning() << "Android MIDI I/O thread did not stop within 2 seconds";
            m_pIoThread->wait();
        }
        delete m_pIoThread;
        m_pIoThread = nullptr;
    }

    if (isOpen()) {
        setOpen(false);
    }
    return baseResult;
}

bool AndroidMidiController::poll() {
    return m_usingMidiManager || (m_pIoThread && m_pIoThread->isRunning());
}

bool AndroidMidiController::isPolling() const {
    return m_usingMidiManager || (m_pIoThread && m_pIoThread->isRunning());
}

void AndroidMidiController::sendShortMsg(
        unsigned char status, unsigned char byte1, unsigned char byte2) {
    QByteArray data(kMidiMsgSize, '\0');
    data[0] = static_cast<char>(status);
    data[1] = static_cast<char>(byte1);
    data[2] = static_cast<char>(byte2);
    sendBytes(data);
}

bool AndroidMidiController::sendBytes(const QByteArray& data) {
    if (m_usingMidiManager && m_midiHelper.isValid()) {
        QJniEnvironment env;
        jbyteArray jdata = env->NewByteArray(data.size());
        if (!jdata) {
            return false;
        }
        env->SetByteArrayRegion(jdata,
                0,
                data.size(),
                reinterpret_cast<const jbyte*>(data.constData()));
        m_midiHelper.callMethod<void>("send",
                "([BII)V",
                jdata,
                static_cast<jint>(0),
                static_cast<jint>(data.size()));
        env->DeleteLocalRef(jdata);
        return true;
    }

    if (m_pIoThread && m_pIoThread->isRunning()) {
        m_pIoThread->send(data);
        return true;
    }
    return false;
}

void AndroidMidiController::receiveAndroidMidi(const QByteArray& data) {
    // Android MidiReceiver supplies normal MIDI bytes (not USB-MIDI 4-byte
    // event packets). Parse channel voice messages and feed Mixxx's normal
    // MIDI input path so mappings and the Learning Wizard see them.
    int pos = 0;
    while (pos < data.size()) {
        const auto status = static_cast<unsigned char>(data.at(pos));

        if (status < 0x80) {
            // Running status is uncommon for Android's USB MIDI translation.
            // Skip orphan data rather than inventing a status byte.
            ++pos;
            continue;
        }

        if (status >= 0x80 && status <= 0xEF) {
            const unsigned char opcode = status & 0xF0;
            const int messageLength =
                    (opcode == 0xC0 || opcode == 0xD0) ? 2 : 3;
            if (pos + messageLength > data.size()) {
                break;
            }

            const auto control =
                    static_cast<unsigned char>(data.at(pos + 1));
            const auto value = messageLength == 3
                    ? static_cast<unsigned char>(data.at(pos + 2))
                    : static_cast<unsigned char>(0);
            receivedShortMessage(status,
                    control,
                    value,
                    mixxx::Duration::fromMillis(0));
            pos += messageLength;
            continue;
        }

        if (status == 0xF0) {
            const int end = data.indexOf(static_cast<char>(0xF7), pos + 1);
            if (end >= 0) {
                receive(data.mid(pos, end - pos + 1),
                        mixxx::Duration::fromMillis(0));
                pos = end + 1;
            } else {
                receive(data.mid(pos), mixxx::Duration::fromMillis(0));
                break;
            }
            continue;
        }

        // Ignore real-time/system-common bytes here. They are not needed for
        // controller mappings and can otherwise flood MIDI learn.
        ++pos;
    }
}

// ── IoThread ────────────────────────────────────────────────────

AndroidMidiController::IoThread::IoThread(
        const QJniObject& usbDevice,
        jint usbFd,
        int interfaceNumber,
        AndroidMidiController* controller)
        : m_usbDevice(usbDevice),
          m_usbFd(usbFd),
          m_interfaceNumber(interfaceNumber),
          m_controller(controller) {
}

void AndroidMidiController::IoThread::stop() {
    m_stop.storeRelaxed(1);
}

void AndroidMidiController::IoThread::send(const QByteArray& data) {
    QMutexLocker lock(&m_sendMutex);
    m_pendingSend = data;
    m_hasPending = true;
}

void AndroidMidiController::IoThread::processUsbMidiPacket(
        const unsigned char* packet, int len) {
    // USB MIDI packet: 4-byte words: [CIN|MIDI_0|MIDI_1|MIDI_2]
    // CIN (Code Index Number) tells us the MIDI message type and cable number
    for (int i = 0; i + 3 < len; i += 4) {
        unsigned char cin = packet[i] & 0x0F;
        unsigned char midi0 = packet[i + 1];
        unsigned char midi1 = packet[i + 2];
        unsigned char midi2 = packet[i + 3];

        if (cin == 0x0) {
            continue; // miscellaneous / reserved
        }

        // Standard MIDI messages in USB: cin 0x2—0xF map to MIDI status bytes
        // cin 0x8 = Note Off, 0x9 = Note On, 0xA = Poly Pressure,
        // 0xB = CC, 0xC = Program Change, 0xD = Channel Pressure,
        // 0xE = Pitch Bend, 0xF = System
        m_controller->receive(QByteArray(1, static_cast<char>(midi0)),
                mixxx::Duration::fromMillis(0));
        if (cin >= 0x8 && cin <= 0xE) {
            // 3-byte message: [status+channel, data1, data2]
            unsigned char statusByte = (cin << 4) | (midi0 & 0x0F);
            m_controller->receivedShortMessage(
                    statusByte, midi1, midi2, mixxx::Duration::fromMillis(0));
        } else if (cin == 0x2 || cin == 0x3 || cin == 0x4 || cin == 0x6 || cin == 0x7) {
            // 2-byte system common messages (MTC, Song Position, Song Select, etc.)
            m_controller->receive(
                    QByteArray(1, static_cast<char>(midi0)),
                    mixxx::Duration::fromMillis(0));
        } else if (cin == 0x5) {
            // System exclusive — variable length
            // For now just forward the raw bytes
            QByteArray sysex;
            sysex.append(static_cast<char>(0xF0));
            int sysexLen = (midi0 == 0xF0) ? 3 : 0;
            Q_UNUSED(sysexLen);
            m_controller->receive(
                    QByteArray(1, static_cast<char>(midi0)),
                    mixxx::Duration::fromMillis(0));
        }
    }
}

void AndroidMidiController::IoThread::run() {
    __android_log_print(ANDROID_LOG_INFO,
            "mixxx",
            "AndroidMidiController I/O thread starting, FD=%d",
            m_usbFd);

    QJniEnvironment env;
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    auto usbManager = context.callObjectMethod("getSystemService",
            "(Ljava/lang/String;)Ljava/lang/Object;",
            QJniObject::getStaticObjectField("android/content/Context",
                    "USB_SERVICE",
                    "Ljava/lang/String;")
                    .object());

    auto usbConnection = usbManager.callObjectMethod("openDevice",
            "(Landroid/hardware/usb/UsbDevice;)Landroid/hardware/usb/UsbDeviceConnection;",
            m_usbDevice.object());

    if (!usbConnection.isValid()) {
        __android_log_print(ANDROID_LOG_ERROR, "mixxx", "IoThread: openDevice failed");
        return;
    }

    auto usbInterface = m_usbDevice.callObjectMethod("getInterface",
            "(I)Landroid/hardware/usb/UsbInterface;",
            m_interfaceNumber);

    // Claim the MIDI interface on this thread's USB connection
    if (usbInterface.isValid()) {
        jboolean claimed = usbConnection.callMethod<jboolean>("claimInterface",
                "(Landroid/hardware/usb/UsbInterface;Z)Z",
                usbInterface.object(),
                true);
        if (!claimed) {
            __android_log_print(ANDROID_LOG_WARN,
                    "mixxx",
                    "IoThread: claimInterface failed — trying bulkTransfer "
                    "anyway");
            // Fall through — some devices work without explicit claim
        }
    }

    // Scan endpoints by direction instead of hardcoded indices
    QJniObject bulkInEndpoint;
    QJniObject bulkOutEndpoint;
    jint epCount = usbInterface.callMethod<jint>("getEndpointCount");
    for (jint ep = 0; ep < epCount; ep++) {
        auto endpoint = usbInterface.callObjectMethod("getEndpoint",
                "(I)Landroid/hardware/usb/UsbEndpoint;",
                ep);
        jint dir = endpoint.callMethod<jint>("getDirection");
        // DIRECTION_IN = 0x80, DIRECTION_OUT = 0x00 in UsbConstants
        if (dir == 0x80 && !bulkInEndpoint.isValid()) {
            bulkInEndpoint = endpoint;
        } else if (dir == 0x00 && !bulkOutEndpoint.isValid()) {
            bulkOutEndpoint = endpoint;
        }
    }

    if (!bulkInEndpoint.isValid() && !bulkOutEndpoint.isValid()) {
        __android_log_print(ANDROID_LOG_ERROR,
                "mixxx",
                "IoThread: no bulk endpoints found on interface %d "
                "(epCount=%d)",
                m_interfaceNumber,
                epCount);
        return;
    }

    __android_log_print(ANDROID_LOG_INFO,
            "mixxx",
            "IoThread: endpoints — IN=%s OUT=%s",
            bulkInEndpoint.isValid() ? "yes" : "no",
            bulkOutEndpoint.isValid() ? "yes" : "no");

    jbyteArray readBuffer = env->NewByteArray(kUsbMidiPacketSize);

    while (!m_stop.loadRelaxed()) {
        // Process sends
        {
            QMutexLocker lock(&m_sendMutex);
            if (m_hasPending && bulkOutEndpoint.isValid()) {
                jbyteArray writeBuf = env->NewByteArray(m_pendingSend.size());
                env->SetByteArrayRegion(writeBuf,
                        0,
                        m_pendingSend.size(),
                        reinterpret_cast<const jbyte*>(
                                m_pendingSend.constData()));
                usbConnection.callMethod<jint>("bulkTransfer",
                        "(Landroid/hardware/usb/UsbEndpoint;[BII)I",
                        bulkOutEndpoint.object(),
                        writeBuf,
                        0,
                        m_pendingSend.size(),
                        kPollTimeoutMs);
                env->DeleteLocalRef(writeBuf);
                m_hasPending = false;
            }
        }

        // Read incoming data
        jint bytesRead = usbConnection.callMethod<jint>("bulkTransfer",
                "(Landroid/hardware/usb/UsbEndpoint;[BII)I",
                bulkInEndpoint.object(),
                readBuffer,
                0,
                kUsbMidiPacketSize,
                kPollTimeoutMs);

        if (bytesRead > 0) {
            jbyte* elements = env->GetByteArrayElements(readBuffer, nullptr);
            if (elements) {
                processUsbMidiPacket(
                        reinterpret_cast<const unsigned char*>(elements),
                        bytesRead);
                env->ReleaseByteArrayElements(readBuffer, elements, JNI_ABORT);
            }
        }

        msleep(5);
    }

    env->DeleteLocalRef(readBuffer);
    __android_log_print(ANDROID_LOG_INFO, "mixxx", "AndroidMidiController I/O thread stopped");
}

#endif // __ANDROID__
