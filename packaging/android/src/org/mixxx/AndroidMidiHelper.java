package org.mixxx;

import android.media.midi.MidiDevice;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiInputPort;
import android.media.midi.MidiManager;
import android.media.midi.MidiOutputPort;
import android.media.midi.MidiReceiver;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public class AndroidMidiHelper {
    private static final String TAG = "MixxxMidi";

    private static native void midiReceive(int controllerId, byte[] data,
        int offset, int count, long timestamp);

    private volatile MidiDevice mDevice;
    private MidiInputPort mInputPort;
    private final List<MidiOutputPort> mOutputPorts = new ArrayList<>();
    private volatile int mControllerId;

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

        // MidiManager.openDevice() is asynchronous. Use a dedicated looper for
        // its callback instead of sleeping on whichever thread called us.
        // This avoids deadlocking when the callback would otherwise be delivered
        // on the same thread that is waiting for it.
        HandlerThread callbackThread = new HandlerThread("MixxxMidiOpen");
        callbackThread.start();
        Handler callbackHandler = new Handler(callbackThread.getLooper());
        CountDownLatch latch = new CountDownLatch(1);

        mgr.openDevice(info, new MidiManager.OnDeviceOpenedListener() {
            @Override
            public void onDeviceOpened(MidiDevice device) {
                mDevice = device;
                latch.countDown();
            }
        }, callbackHandler);

        boolean completed = false;
        try {
            completed = latch.await(5, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } finally {
            callbackThread.quitSafely();
        }

        if (!completed || mDevice == null) {
            Log.e(TAG, "openDevice timed out or failed for " + getDeviceName(info));
            return false;
        }

        return true;
    }

    public boolean openPorts(int inIdx, int outIdx) {
        MidiDevice device = mDevice;
        if (device == null) {
            return false;
        }

        closePortsOnly();
        MidiDeviceInfo info = device.getInfo();

        // For output from the controller to the app, listen to every output
        // port. Multi-port DJ controllers often expose controls on a port other
        // than port 0, which MIDI monitor apps handle automatically.
        for (int i = 0; i < info.getOutputPortCount(); ++i) {
            MidiOutputPort port = device.openOutputPort(i);
            if (port != null) {
                port.connect(mNativeReceiver);
                mOutputPorts.add(port);
                Log.i(TAG, "Listening to MIDI output port " + i);
            } else {
                Log.w(TAG, "Could not open MIDI output port " + i);
            }
        }

        // One device input port is enough for Mixxx LED/output messages.
        if (info.getInputPortCount() > 0) {
            int sendPort = (inIdx >= 0 && inIdx < info.getInputPortCount())
                    ? inIdx : 0;
            mInputPort = device.openInputPort(sendPort);
            if (mInputPort == null) {
                Log.w(TAG, "Could not open MIDI input port " + sendPort);
            }
        }

        return !mOutputPorts.isEmpty() || mInputPort != null;
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

        for (MidiOutputPort port : mOutputPorts) {
            try {
                port.close();
            } catch (IOException e) {
                Log.e(TAG, "output port close failed: " + e.getMessage());
            }
        }
        mOutputPorts.clear();
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
