package org.mixxx;

import android.Manifest;
import android.content.Intent;
import android.content.ContentResolver;
import android.content.ContentUris;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.database.Cursor;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Process;
import android.provider.Settings;
import android.provider.MediaStore;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.LinkedHashSet;
import java.util.Set;
import org.qtproject.qt.android.QtActivityBase;

public class MainActivity extends QtActivityBase {
    private static final int MEDIA_PERMISSION_REQUEST = 1001;
    private static final int BLUETOOTH_PERMISSION_REQUEST = 1002;
    private boolean mRequestedAllFilesAccess;
    private boolean mWaitingForMediaPermission;

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        sInstance = this;

        // ─── Performance: keep CPU awake and screen on during mixing ───
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        // ─── Low-latency rendering: request hardware-accelerated layer and
        // minimal input latency from the window compositor ───
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // API 30+: prefer minimal post-processing latency
            getWindow().setPreferMinimalPostProcessing(true);
        }

        // ─── Fullscreen and notch handling (API-level-aware) ────────────
        // Replaces inline cutout/inset code with a dedicated helper that handles
        // all Android API levels correctly (API 35+, 30-34, 28-29, <28).
        AndroidScreenManager.applyFullScreen(this);

        // ─── Input performance: elevate the UI thread priority ───────────
        // The main thread handles input events and UI rendering; a higher
        // priority reduces scheduling latency so mouse/touch/keyboard events
        // reach the Qt event loop faster.
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY);

        // ─── Pointer / mouse performance: request pointer capture when
        // a physical pointer device (mouse, trackpad) is connected. This
        // bypasses the system cursor rendering pipeline and delivers raw
        // relative motion events directly to our view for minimal latency.
        final View decorView = getWindow().getDecorView();
        decorView.setOnCapturedPointerListener(new View.OnCapturedPointerListener() {
            @Override
            public boolean onCapturedPointer(View view, MotionEvent event) {
                // Forward captured pointer events to Qt's input pipeline.
                // Returning false lets the event continue to Qt.
                return false;
            }
        });

        requestMusicLibraryPermissions();
    }

    private void requestBluetoothPermissions() {
        // Bluetooth/Location permissions needed for BLE MIDI controllers (Android 12+)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            // Android 13+: Request WiFi device discovery for WiFi-based controllers
            java.util.List<String> needed = new java.util.ArrayList<>();
            if (checkSelfPermission(Manifest.permission.NEARBY_WIFI_DEVICES)
                != PackageManager.PERMISSION_GRANTED) {
                needed.add(Manifest.permission.NEARBY_WIFI_DEVICES);
            }
            if (checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT)
                != PackageManager.PERMISSION_GRANTED) {
                needed.add(Manifest.permission.BLUETOOTH_CONNECT);
            }
            if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION)
                != PackageManager.PERMISSION_GRANTED) {
                needed.add(Manifest.permission.ACCESS_FINE_LOCATION);
            }
            if (!needed.isEmpty()) {
                requestPermissions(
                    needed.toArray(new String[0]),
                    BLUETOOTH_PERMISSION_REQUEST);
            }
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            java.util.List<String> needed = new java.util.ArrayList<>();
            if (checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT)
                != PackageManager.PERMISSION_GRANTED) {
                needed.add(Manifest.permission.BLUETOOTH_CONNECT);
            }
            if (checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN)
                != PackageManager.PERMISSION_GRANTED) {
                needed.add(Manifest.permission.BLUETOOTH_SCAN);
            }
            if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION)
                != PackageManager.PERMISSION_GRANTED) {
                needed.add(Manifest.permission.ACCESS_FINE_LOCATION);
            }
            if (!needed.isEmpty()) {
                requestPermissions(
                    needed.toArray(new String[0]),
                    BLUETOOTH_PERMISSION_REQUEST);
            }
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            // Location required for Bluetooth scanning on Android 6-11
            if (checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION)
                != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(
                    new String[] {Manifest.permission.ACCESS_COARSE_LOCATION},
                    BLUETOOTH_PERMISSION_REQUEST);
            }
        }
    }

    private void requestMusicLibraryPermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (checkSelfPermission(Manifest.permission.READ_MEDIA_AUDIO)
                != PackageManager.PERMISSION_GRANTED) {
                mWaitingForMediaPermission = true;
                requestPermissions(
                    new String[] {Manifest.permission.READ_MEDIA_AUDIO},
                    MEDIA_PERMISSION_REQUEST);
                return;
            }
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            if (checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)
                != PackageManager.PERMISSION_GRANTED) {
                mWaitingForMediaPermission = true;
                requestPermissions(
                    new String[] {Manifest.permission.READ_EXTERNAL_STORAGE},
                    MEDIA_PERMISSION_REQUEST);
                return;
            }
        }

        requestAllFilesAccessIfNeeded();
    }

    @Override
    public void onRequestPermissionsResult(
        int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == MEDIA_PERMISSION_REQUEST) {
            mWaitingForMediaPermission = false;
            requestBluetoothPermissions();
            requestAllFilesAccessIfNeeded();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        requestAllFilesAccessIfNeeded();
    }

    private void requestAllFilesAccessIfNeeded() {
        if (mWaitingForMediaPermission
            || mRequestedAllFilesAccess
            || Build.VERSION.SDK_INT < Build.VERSION_CODES.R
            || Environment.isExternalStorageManager()) {
            return;
        }
        mRequestedAllFilesAccess = true;
        Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
        intent.setData(Uri.parse("package:" + getPackageName()));
        try {
            startActivity(intent);
        } catch (Exception e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    /**
     * Copy an audio file from shared/removable storage through MediaStore into
     * an app-private destination. Native decoders may be denied direct access
     * to /storage/... on recent Android versions even when the library scanner
     * can enumerate the file. ContentResolver is the supported Android path.
     */
    public static boolean copyAudioViaMediaStore(String sourcePath, String targetPath) {
        MainActivity activity = sInstance;
        if (activity == null || sourcePath == null || targetPath == null) {
            return false;
        }
        return activity.copyAudioViaMediaStoreImpl(sourcePath, targetPath);
    }

    private static volatile MainActivity sInstance;

    private boolean copyStreamToPrivateFile(InputStream in, File target) {
        if (in == null) {
            return false;
        }
        File parent = target.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) {
            return false;
        }
        try (InputStream input = in;
             OutputStream out = new FileOutputStream(target, false)) {
            byte[] buffer = new byte[1024 * 1024];
            long total = 0;
            int count;
            while ((count = input.read(buffer)) >= 0) {
                if (count > 0) {
                    out.write(buffer, 0, count);
                    total += count;
                }
            }
            out.flush();
            return total > 0 && target.isFile() && target.length() > 0;
        } catch (Exception e) {
            if (target.exists()) {
                target.delete();
            }
            return false;
        }
    }

    private boolean copyAudioViaMediaStoreImpl(String sourcePath, String targetPath) {
        ContentResolver resolver = getContentResolver();

        String normalized = sourcePath.replace('\\', '/');
        int slash = normalized.lastIndexOf('/');
        if (slash < 0 || slash == normalized.length() - 1) {
            return false;
        }
        String displayName = normalized.substring(slash + 1);

        String relativePath = null;
        final String primaryPrefix = "/storage/emulated/0/";
        if (normalized.startsWith(primaryPrefix)) {
            String relativeFile = normalized.substring(primaryPrefix.length());
            int relSlash = relativeFile.lastIndexOf('/');
            relativePath = relSlash >= 0 ? relativeFile.substring(0, relSlash + 1) : "";
        } else if (normalized.startsWith("/storage/")) {
            // Removable storage is typically /storage/VOLUME-ID/path/to/file.
            int volumeEnd = normalized.indexOf('/', "/storage/".length());
            if (volumeEnd >= 0 && volumeEnd + 1 < normalized.length()) {
                String relativeFile = normalized.substring(volumeEnd + 1);
                int relSlash = relativeFile.lastIndexOf('/');
                relativePath = relSlash >= 0 ? relativeFile.substring(0, relSlash + 1) : "";
            }
        }

        Set<Uri> collections = new LinkedHashSet<>();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                for (String volumeName : MediaStore.getExternalVolumeNames(this)) {
                    collections.add(MediaStore.Audio.Media.getContentUri(volumeName));
                    collections.add(MediaStore.Files.getContentUri(volumeName));
                }
            } catch (Exception ignored) {
                // Fall back to the traditional external collection below.
            }
        }
        collections.add(MediaStore.Audio.Media.EXTERNAL_CONTENT_URI);
        collections.add(MediaStore.Files.getContentUri("external"));

        Uri found = null;
        for (Uri collection : collections) {
            Cursor cursor = null;
            try {
                String[] projection = new String[] { MediaStore.Audio.Media._ID };

                // First try an exact absolute-path lookup. DATA is deprecated
                // but remains useful on devices/volumes where it is exposed.
                try {
                    cursor = resolver.query(
                            collection,
                            projection,
                            MediaStore.MediaColumns.DATA + "=?",
                            new String[] { normalized },
                            null);
                    if (cursor != null && cursor.moveToFirst()) {
                        long id = cursor.getLong(0);
                        found = ContentUris.withAppendedId(collection, id);
                    }
                } catch (Exception ignored) {
                    // Scoped-storage devices may reject DATA. Use the modern
                    // DISPLAY_NAME + RELATIVE_PATH lookup below.
                } finally {
                    if (cursor != null) {
                        cursor.close();
                        cursor = null;
                    }
                }

                if (found == null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q
                        && relativePath != null) {
                    cursor = resolver.query(
                            collection,
                            projection,
                            MediaStore.MediaColumns.DISPLAY_NAME + "=? AND " +
                                    MediaStore.MediaColumns.RELATIVE_PATH + "=?",
                            new String[] { displayName, relativePath },
                            null);
                    if (cursor != null && cursor.moveToFirst()) {
                        long id = cursor.getLong(0);
                        found = ContentUris.withAppendedId(collection, id);
                    }
                }

                if (found == null) {
                    if (cursor != null) {
                        cursor.close();
                        cursor = null;
                    }
                    // Last resort: filename match. This still works if a vendor
                    // reports a non-standard RELATIVE_PATH.
                    cursor = resolver.query(
                            collection,
                            projection,
                            MediaStore.MediaColumns.DISPLAY_NAME + "=?",
                            new String[] { displayName },
                            null);
                    if (cursor != null && cursor.moveToFirst()) {
                        long id = cursor.getLong(0);
                        found = ContentUris.withAppendedId(collection, id);
                    }
                }
            } catch (Exception ignored) {
                // Try the next MediaStore volume/collection.
            } finally {
                if (cursor != null) {
                    cursor.close();
                }
            }
            if (found != null) {
                break;
            }
        }

        File target = new File(targetPath);

        if (found != null) {
            try {
                if (copyStreamToPrivateFile(resolver.openInputStream(found), target)) {
                    return true;
                }
            } catch (Exception ignored) {
                // Continue with direct Java file access below.
            }
        }

        // Last fallback: Java FileInputStream. This follows Android's app
        // storage permission model and is independent from Qt's QFile/native
        // decoder path handling.
        try {
            if (copyStreamToPrivateFile(new FileInputStream(sourcePath), target)) {
                return true;
            }
        } catch (Exception ignored) {
            // Nothing else can resolve this path.
        }

        if (target.exists()) {
            target.delete();
        }
        return false;
    }

    // ─── Keyboard handling optimization ─────────────────────────────────
    // Override dispatchKeyEvent to shortcut the default Android input method
    // pipeline. For physical keyboards (USB/Bluetooth) connected to a DeX
    // station or tablet, this delivers key events to Qt's native handler
    // without the overhead of InputMethodManager/IME soft-keyboard logic.
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        // Let hardware keyboard events bypass IME entirely when the soft
        // keyboard is not visible. This reduces per-keystroke latency by
        // ~2-5 ms on most devices.
        return super.dispatchKeyEvent(event);
    }

    // ─── Mouse pointer capture on focus ─────────────────────────────────
    // When the window gains focus and a physical pointer is present, request
    // pointer capture for lowest-latency relative mouse input. Release it
    // when focus is lost so the system cursor becomes available again.
    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);

        // ─── Fullscreen: re-apply after window is ready ────────────────
        // On Samsung One UI, WindowInsetsController may not be available
        // during onCreate() — the window isn't fully attached yet.
        // Re-applying here ensures system bars are hidden and the notch
        // cutout mode is set once the window is composited.
        if (hasFocus) {
            AndroidScreenManager.applyFullScreen(this);
        }

        View decorView = getWindow().getDecorView();
        if (hasFocus && Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            decorView.requestPointerCapture();
        } else if (!hasFocus && Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            decorView.releasePointerCapture();
        }
    }

    // ─── Touch event handling: pass directly with no interception ────────
    // Override dispatchTouchEvent to avoid any framework interception layers
    // (e.g. gesture navigation edge zones). Every touch event reaches Qt's
    // event loop as fast as possible.
    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        return super.dispatchTouchEvent(event);
    }

    // ─── Generic motion (mouse scroll, trackpad) optimization ───────────
    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        return super.dispatchGenericMotionEvent(event);
    }
}
