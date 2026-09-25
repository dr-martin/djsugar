#pragma once

#ifdef __ANDROID__

#include <QAtomicInt>
#include <QByteArray>
#include <QJniObject>
#include <QMutex>
#include <QString>
#include <QThread>

#include "controllers/midi/midicontroller.h"

/// Android MIDI controller.
///
/// Devices reported by Android MidiManager use Android's native MIDI API.
/// UsbManager bulk I/O remains as a fallback for devices MidiManager cannot see.
class AndroidMidiController : public MidiController {
    Q_OBJECT
  public:
    AndroidMidiController(const QString& name,
            const QJniObject& midiDeviceInfo,
            const QJniObject& usbDevice,
            int interfaceNumber,
            int inputPortIndex,
            int outputPortIndex,
            uint16_t vendorId,
            uint16_t productId,
            const QString& vendorStr,
            const QString& productStr);
    ~AndroidMidiController() override;

    /// Called from the JNI MidiReceiver bridge on this controller's Qt thread.
    void receiveAndroidMidi(const QByteArray& data);

    PhysicalTransportProtocol getPhysicalTransportProtocol() const override {
        return PhysicalTransportProtocol::USB;
    }
    QString getVendorString() const override {
        return m_vendor;
    }
    QString getProductString() const override {
        return m_product;
    }
    std::optional<uint16_t> getVendorId() const override {
        return m_vendorId ? std::optional<uint16_t>(m_vendorId) : std::nullopt;
    }
    std::optional<uint16_t> getProductId() const override {
        return m_productId ? std::optional<uint16_t>(m_productId) : std::nullopt;
    }
    QString getSerialNumber() const override {
        return {};
    }
    std::optional<uint8_t> getUsbInterfaceNumber() const override {
        return m_interfaceNumber >= 0
                ? std::optional<uint8_t>(static_cast<uint8_t>(m_interfaceNumber))
                : std::nullopt;
    }

  protected:
    void sendShortMsg(unsigned char status,
            unsigned char byte1,
            unsigned char byte2) override;

  private:
    int open(const QString& resourcePath) override;
    int close() override;
    bool sendBytes(const QByteArray& data) override;
    bool poll() override;
    bool isPolling() const override;

    int openWithMidiManager();
    int openWithUsbBulk();

    class IoThread : public QThread {
      public:
        IoThread(const QJniObject& usbDevice,
                jint usbFd,
                int interfaceNumber,
                AndroidMidiController* controller);
        void stop();
        void send(const QByteArray& data);

      protected:
        void run() override;

      private:
        void processUsbMidiPacket(const unsigned char* packet, int len);

        QJniObject m_usbDevice;
        jint m_usbFd;
        int m_interfaceNumber;
        AndroidMidiController* m_controller;
        QAtomicInt m_stop{0};
        QMutex m_sendMutex;
        QByteArray m_pendingSend;
        bool m_hasPending{false};
    };

    IoThread* m_pIoThread{nullptr};

    QJniObject m_midiDeviceInfo;
    QJniObject m_midiHelper;
    QJniObject m_usbDevice;

    int m_interfaceNumber{-1};
    int m_inputPortIndex{-1};
    int m_outputPortIndex{-1};
    int m_controllerId{-1};
    bool m_usingMidiManager{false};

    uint16_t m_vendorId{0};
    uint16_t m_productId{0};
    QString m_vendor;
    QString m_product;
};

#endif // __ANDROID__
