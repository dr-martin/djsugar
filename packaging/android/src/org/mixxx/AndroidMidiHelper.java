package org.mixxx;

import android.media.midi.MidiDevice;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiInputPort;
import android.media.midi.MidiManager;
import android.media.midi.MidiOutputPort;
import android.media.midi.MidiReceiver;
import android.os.Bundle;
import android.util.Log;
import java.io.IOException;

public class AndroidMidiHelper {
    private static final String TAG = "MixxxMidi";

    private static native void midiReceive(int controllerId, byte[] data,
        int offset, int count, long timestamp);

    private volatile MidiDevice mDevice;
    private MidiInputPort mInputPort;
    private MidiOutputPort mOutputPort;
    private volatile int mControllerId;
    private volatile boolean mOpenDone;

    /**
     * Custom receiver that forwards incoming MIDI data to native code.
     */
    private final MidiReceiver mNativeReceiver = new MidiReceiver() {
        @Override
        public void onSend(byte[] data, int offset, int count, long timestamp) {
            midiReceive(mControllerId, data, offset, count, timestamp);
        }
    };

    // Static helpers used by C++ enumerator
    @SuppressWarnings("deprecation")
    public static MidiDeviceInfo[] getDevices(MidiManager mgr) {
        return mgr.getDevices();
    }

    public static String getDeviceName(MidiDeviceInfo info) {
        Bundle p = info.getProperties();
        String n = p.getString(MidiDeviceInfo.PROPERTY_NAME);
        return n != null ? n : "Unknown";
    }

    // Instance methods for device/port management
    public boolean open(MidiManager mgr, MidiDeviceInfo info, int controllerId) {
        mControllerId = controllerId;
        mOpenDone = false;
        mgr.openDevice(info, new MidiManager.OnDeviceOpenedListener() {
            @Override
            public void onDeviceOpened(MidiDevice device) {
                mDevice = device;
                mOpenDone = true;
            }
        }, null);
        long deadline = System.currentTimeMillis() + 3000;
        while (!mOpenDone && System.currentTimeMillis() < deadline) {
            try {
                Thread.sleep(10);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }
        }
        return mDevice != null;
    }

    public boolean openPorts(int inIdx, int outIdx) {
        MidiDevice device = mDevice;
        if (device == null) {
            return false;
        }

        MidiDeviceInfo info = device.getInfo();

        // A MidiInputPort is an input *to the device* (app -> controller).
        if (inIdx >= 0) {
            if (inIdx >= info.getInputPortCount()) {
                return false;
            }
            mInputPort = device.openInputPort(inIdx);
            if (mInputPort == null) {
                return false;
            }
        }

        // A MidiOutputPort is output *from the device* (controller -> app).
        // Connect it to our receiver so button/jog/fader messages reach JNI.
        if (outIdx >= 0) {
            if (outIdx >= info.getOutputPortCount()) {
                closePortsOnly();
                return false;
            }
            mOutputPort = device.openOutputPort(outIdx);
            if (mOutputPort == null) {
                closePortsOnly();
                return false;
            }
            mOutputPort.connect(mNativeReceiver);
        }

        return inIdx >= 0 || outIdx >= 0;
    }

    private void closePortsOnly() {
        if (mInputPort != null) {
            try {
                mInputPort.close();
            } catch (IOException e) {
                Log.e(TAG, "input port close failed: " + e.getMessage());
            }
            mInputPort = null;
        }
        if (mOutputPort != null) {
            try {
                mOutputPort.close();
            } catch (IOException e) {
                Log.e(TAG, "output port close failed: " + e.getMessage());
            }
            mOutputPort = null;
        }
    }

    public void send(byte[] data, int offset, int count) {
        if (mInputPort == null) {
            return;
        }
        try {
            mInputPort.send(data, offset, count, 0);
        } catch (IOException e) {
            Log.e(TAG, "send failed: " + e.getMessage());
        }
    }

    public void close() {
        closePortsOnly();

        MidiDevice device = mDevice;
        mDevice = null;
        if (device != null) {
            try {
                device.close();
            } catch (IOException e) {
                Log.e(TAG, "device close failed: " + e.getMessage());
            }
        }
    }
}
