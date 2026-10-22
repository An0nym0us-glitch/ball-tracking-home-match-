package com.pocketeye.camera;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.hardware.Camera;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.SystemClock;
import android.text.InputType;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.EditText;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.List;

/**
 * PocketEye Camera: films the pitch and streams it to the laptop, which does all
 * the tracking. Built for old phones (Android 5.0+, 1 GB RAM): Camera1 API, no
 * support libraries. Frames carry the time they were captured.
 */
public class MainActivity extends Activity implements SurfaceHolder.Callback, Camera.PreviewCallback,
        StreamClient.Listener {

    private static final int REQ_CAMERA = 1;
    private static final String[] SIZES = {"1280x720", "960x540", "640x480"};
    private static final int[] QUALITIES = {60, 70, 80};

    private SurfaceView preview;
    private TextView status, card;
    private Camera camera;
    private int camW, camH, camFps;
    private float hfov = 60f;
    // Frames come in sensor orientation. In the (fixed) landscape screen they
    // are upright when the sensor is mounted at 90 degrees (almost all phones);
    // a sensor at 270 gives upside-down frames, turned before sending.
    private boolean rotate180;
    private boolean surfaceReady;
    private volatile StreamClient client;
    private volatile boolean scanning;
    private volatile boolean decoding;
    private long lastDecode;
    private final QrDecoder qr = new QrDecoder();
    private final Handler ui = new Handler();
    private SharedPreferences prefs;
    private int capturedSinceStats;
    private long statsStart;
    private int lastSent;
    private long lastBytes;
    private String connection = "";
    private JSONObject lastLaptop;
    private boolean userStopped = true;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setContentView(R.layout.activity_main);
        prefs = getSharedPreferences("pocketeye", MODE_PRIVATE);
        preview = (SurfaceView) findViewById(R.id.preview);
        status = (TextView) findViewById(R.id.status);
        card = (TextView) findViewById(R.id.card);
        preview.getHolder().addCallback(this);
        findViewById(R.id.scan).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startScan();
            }
        });
        findViewById(R.id.manual).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                askAddress();
            }
        });
        findViewById(R.id.settings).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                askQuality();
            }
        });
        try {
            String saved = prefs.getString("laptop", null);
            if (saved != null) lastLaptop = new JSONObject(saved);
        } catch (Exception ignored) {
            lastLaptop = null;
        }
        setStatus(lastLaptop == null ? "Open the laptop app (Live mode) and press \"Scan laptop QR\"."
                : "Press \"Scan laptop QR\", or wait: reconnecting to the last laptop...");
        if (lastLaptop != null) {
            userStopped = false;
            ui.postDelayed(new Runnable() {
                @Override
                public void run() {
                    connect(lastLaptop);
                }
            }, 1500);
        }
        ui.postDelayed(statsTick, 1000);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (Build.VERSION.SDK_INT >= 23
                && checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.CAMERA}, REQ_CAMERA);
        } else if (surfaceReady) {
            openCamera();
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        closeCamera();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        userStopped = true;
        StreamClient c = client;
        if (c != null) c.close();
        ui.removeCallbacksAndMessages(null);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode == REQ_CAMERA && grantResults.length > 0
                && grantResults[0] == PackageManager.PERMISSION_GRANTED && surfaceReady) {
            openCamera();
        } else if (requestCode == REQ_CAMERA) {
            setStatus("Camera permission is needed.");
        }
    }

    // ---------------------------------------------------------------- camera

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        surfaceReady = true;
        if (Build.VERSION.SDK_INT < 23
                || checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            openCamera();
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        closeCamera();
    }

    private void openCamera() {
        if (camera != null) return;
        try {
            int id = 0;
            Camera.CameraInfo info = new Camera.CameraInfo();
            for (int i = 0; i < Camera.getNumberOfCameras(); i++) {
                Camera.getCameraInfo(i, info);
                if (info.facing == Camera.CameraInfo.CAMERA_FACING_BACK) {
                    id = i;
                    break;
                }
            }
            Camera.getCameraInfo(id, info);
            camera = Camera.open(id);
            Camera.Parameters p = camera.getParameters();
            choosePreviewSize(p);
            chooseFps(p);
            List<String> focus = p.getSupportedFocusModes();
            if (focus != null && focus.contains(Camera.Parameters.FOCUS_MODE_CONTINUOUS_VIDEO)) {
                p.setFocusMode(Camera.Parameters.FOCUS_MODE_CONTINUOUS_VIDEO);
            }
            p.setRecordingHint(true);
            if (p.isVideoStabilizationSupported()) p.setVideoStabilization(false);
            camera.setParameters(p);
            hfov = p.getHorizontalViewAngle();
            camera.setDisplayOrientation(displayOrientation(info));
            rotate180 = (info.orientation - 90 + 360) % 360 == 180;
            camera.setPreviewDisplay(preview.getHolder());
            int size = camW * camH * 3 / 2;
            for (int i = 0; i < 4; i++) camera.addCallbackBuffer(new byte[size]);
            camera.setPreviewCallbackWithBuffer(this);
            camera.startPreview();
            statsStart = SystemClock.uptimeMillis();
            capturedSinceStats = 0;
            setStatus("Camera " + camW + "x" + camH + " @ " + camFps + " fps. " + connectionText());
        } catch (Exception e) {
            setStatus("Camera error: " + e.getMessage());
            closeCamera();
        }
    }

    private void choosePreviewSize(Camera.Parameters p) {
        String wanted = prefs.getString("size", SIZES[0]);
        List<Camera.Size> sizes = p.getSupportedPreviewSizes();
        Camera.Size best = null;
        // The wanted size, else the largest 16:9 / 4:3 size not above it.
        int ww = Integer.parseInt(wanted.split("x")[0]);
        for (Camera.Size s : sizes) {
            if (s.width == ww && wanted.equals(s.width + "x" + s.height)) best = s;
        }
        if (best == null) {
            for (Camera.Size s : sizes) {
                if (s.width <= ww && (best == null || s.width * s.height > best.width * best.height)) best = s;
            }
        }
        if (best == null) best = sizes.get(0);
        p.setPreviewSize(best.width, best.height);
        camW = best.width;
        camH = best.height;
    }

    private void chooseFps(Camera.Parameters p) {
        // A fixed 30 fps range keeps frame times regular (no slow-down in low light).
        int[] best = null;
        for (int[] r : p.getSupportedPreviewFpsRange()) {
            int max = r[Camera.Parameters.PREVIEW_FPS_MAX_INDEX];
            int min = r[Camera.Parameters.PREVIEW_FPS_MIN_INDEX];
            if (max > 30000) continue;
            if (best == null || max > best[1] || (max == best[1] && min > best[0])) best = new int[]{min, max};
        }
        if (best != null) {
            p.setPreviewFpsRange(best[0], best[1]);
            camFps = best[1] / 1000;
        } else {
            camFps = 30;
        }
    }

    private int displayOrientation(Camera.CameraInfo info) {
        int rotation = getWindowManager().getDefaultDisplay().getRotation();
        int degrees = 0;
        switch (rotation) {
            case Surface.ROTATION_90: degrees = 90; break;
            case Surface.ROTATION_180: degrees = 180; break;
            case Surface.ROTATION_270: degrees = 270; break;
            default: break;
        }
        return (info.orientation - degrees + 360) % 360;
    }

    private void closeCamera() {
        if (camera == null) return;
        camera.setPreviewCallbackWithBuffer(null);
        camera.stopPreview();
        camera.release();
        camera = null;
    }

    @Override
    public void onPreviewFrame(byte[] data, Camera cam) {
        long timeUs = SystemClock.elapsedRealtimeNanos() / 1000;
        capturedSinceStats++;
        StreamClient c = client;
        if (c != null && c.isRunning()) c.offerFrame(data, timeUs);
        if (scanning && !decoding && SystemClock.uptimeMillis() - lastDecode > 300) {
            decoding = true;
            lastDecode = SystemClock.uptimeMillis();
            final byte[] copy = data.clone();
            final int w = camW, h = camH;
            new Thread(new Runnable() {
                @Override
                public void run() {
                    String text = qr.decode(copy, w, h);
                    decoding = false;
                    if (text != null) onQr(text);
                }
            }).start();
        }
        cam.addCallbackBuffer(data);
    }

    // ------------------------------------------------------------ connection

    private void startScan() {
        StreamClient c = client;
        if (c != null) c.close();
        client = null;
        userStopped = true;
        scanning = true;
        setStatus("Point the phone at the QR code on the laptop screen...");
    }

    private void onQr(String text) {
        try {
            final JSONObject j = new JSONObject(text);
            if (!"pocketeye".equals(j.optString("app"))) return;
            scanning = false;
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    userStopped = false;
                    connect(j);
                }
            });
        } catch (Exception ignored) {
            // not our QR code
        }
    }

    private void askAddress() {
        final EditText input = new EditText(this);
        input.setHint("192.168.1.20 47800 123456");
        input.setInputType(InputType.TYPE_CLASS_TEXT);
        new AlertDialog.Builder(this)
                .setTitle("Laptop address, port and code (shown under the QR code)")
                .setView(input)
                .setPositiveButton("Connect", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        String[] parts = input.getText().toString().trim().split("[\\s:]+");
                        if (parts.length < 3) {
                            setStatus("Type: address port code");
                            return;
                        }
                        try {
                            JSONObject j = new JSONObject();
                            j.put("hosts", new JSONArray().put(parts[0]));
                            j.put("port", Integer.parseInt(parts[1]));
                            j.put("token", parts[2]);
                            scanning = false;
                            userStopped = false;
                            connect(j);
                        } catch (Exception e) {
                            setStatus("Bad address: " + e.getMessage());
                        }
                    }
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void askQuality() {
        final String[] items = new String[SIZES.length * QUALITIES.length];
        int k = 0;
        for (String s : SIZES) {
            for (int q : QUALITIES) items[k++] = s + ", JPEG quality " + q;
        }
        new AlertDialog.Builder(this)
                .setTitle("Picture size (smaller = less Wi-Fi, less accuracy)")
                .setItems(items, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        prefs.edit()
                                .putString("size", SIZES[which / QUALITIES.length])
                                .putInt("quality", QUALITIES[which % QUALITIES.length])
                                .apply();
                        closeCamera();
                        openCamera();
                        JSONObject last = lastLaptop;
                        StreamClient c = client;
                        if (c != null) c.close();
                        client = null;
                        if (last != null && !userStopped) connect(last);
                    }
                })
                .show();
    }

    private void connect(final JSONObject laptop) {
        if (camera == null) {
            setStatus("Waiting for the camera...");
            ui.postDelayed(new Runnable() {
                @Override
                public void run() {
                    connect(laptop);
                }
            }, 1000);
            return;
        }
        final JSONArray hosts = laptop.optJSONArray("hosts");
        final int port = laptop.optInt("port", 47800);
        final String token = laptop.optString("token");
        if (hosts == null || hosts.length() == 0) return;
        setStatus("Connecting to the laptop...");
        final StreamClient c = new StreamClient(token, camW, camH, camFps, hfov,
                prefs.getInt("quality", 70), Build.MANUFACTURER + " " + Build.MODEL, this);
        c.setRotate180(rotate180);
        new Thread(new Runnable() {
            @Override
            public void run() {
                if (c.connect(hosts, port)) {
                    client = c;
                    lastLaptop = laptop;
                    prefs.edit().putString("laptop", laptop.toString()).apply();
                } else {
                    setStatus("Laptop not found. Same Wi-Fi? Laptop app in Live mode? Retrying...");
                    retryLater();
                }
            }
        }).start();
    }

    private void retryLater() {
        ui.postDelayed(new Runnable() {
            @Override
            public void run() {
                JSONObject last = lastLaptop;
                if (!userStopped && client == null && last != null) connect(last);
            }
        }, 3000);
    }

    @Override
    public void onConnected(final String laptop) {
        connection = "Streaming to " + laptop;
        setStatus(connection);
    }

    @Override
    public void onResult(final String text, final String decision) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                card.setText(text);
                int color = Color.WHITE;
                if ("OUT".equals(decision)) color = Color.rgb(255, 120, 120);
                else if ("NOT OUT".equals(decision)) color = Color.rgb(140, 230, 140);
                else if (decision != null && decision.startsWith("UMPIRE")) color = Color.rgb(255, 200, 90);
                card.setTextColor(color);
                card.setVisibility(View.VISIBLE);
                ui.removeCallbacks(hideCard);
                ui.postDelayed(hideCard, 20000);
            }
        });
    }

    private final Runnable hideCard = new Runnable() {
        @Override
        public void run() {
            card.setVisibility(View.GONE);
        }
    };

    @Override
    public void onClosed(String reason) {
        client = null;
        connection = "";
        setStatus("Disconnected (" + reason + "). Reconnecting...");
        retryLater();
    }

    private String connectionText() {
        return connection.isEmpty() ? "Not connected." : connection;
    }

    private final Runnable statsTick = new Runnable() {
        @Override
        public void run() {
            long now = SystemClock.uptimeMillis();
            double secs = Math.max(0.5, (now - statsStart) / 1000.0);
            StreamClient c = client;
            if (camera != null && c != null && c.isRunning()) {
                int sent = c.framesSent;
                long bytes = c.bytesSent;
                double camFpsNow = capturedSinceStats / secs;
                double sentFps = (sent - lastSent) / secs;
                double mbps = (bytes - lastBytes) * 8 / 1e6 / secs;
                lastSent = sent;
                lastBytes = bytes;
                int drop = c.framesDropped;
                status.setText(String.format("%s\nCamera %dx%d  %.0f fps  |  sent %.0f fps  %.1f Mbit/s  |  dropped %d",
                        connection, camW, camH, camFpsNow, sentFps, mbps, drop));
            }
            capturedSinceStats = 0;
            statsStart = now;
            ui.postDelayed(this, 1000);
        }
    };

    private void setStatus(final String text) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                status.setText(text);
            }
        });
    }
}
