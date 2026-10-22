package com.pocketeye.camera;

import android.graphics.Rect;
import android.graphics.YuvImage;
import android.os.SystemClock;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedOutputStream;
import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.ArrayDeque;
import java.util.TreeMap;

/**
 * Streams camera frames to the laptop (see app/lib/net/protocol.dart):
 * a JSON hello line, then 'PEF1' + capture time (us) + length + JPEG per frame.
 * Two threads encode JPEG (the slow part on old phones); a sender thread writes
 * the frames in capture order; a receiver thread answers pings and shows the
 * result cards the laptop sends back.
 */
public final class StreamClient {

    public interface Listener {
        void onConnected(String laptop);

        void onResult(String text, String decision);

        void onClosed(String reason);
    }

    private static final int POOL = 6;

    private final String token;
    private final int width, height, fps, quality;
    private final float hfov;
    private final String device;
    private final Listener listener;

    private Socket socket;
    private DataOutputStream out;
    private volatile boolean running;
    private volatile boolean rotate180;

    private final ArrayDeque<byte[]> freeBuffers = new ArrayDeque<>();
    private final ArrayDeque<Job> jobs = new ArrayDeque<>();
    private final TreeMap<Long, Encoded> encoded = new TreeMap<>();
    private long nextSeqIn = 0;
    private long nextSeqOut = 0;

    // Statistics (read by the UI).
    public volatile int framesSent;
    public volatile int framesDropped;
    public volatile long bytesSent;

    private static final class Job {
        final long seq, timeUs;
        final byte[] nv21;

        Job(long seq, long timeUs, byte[] nv21) {
            this.seq = seq;
            this.timeUs = timeUs;
            this.nv21 = nv21;
        }
    }

    private static final class Encoded {
        final long timeUs;
        final byte[] jpeg; // null = encoding failed, skip

        Encoded(long timeUs, byte[] jpeg) {
            this.timeUs = timeUs;
            this.jpeg = jpeg;
        }
    }

    public StreamClient(String token, int width, int height, int fps, float hfov, int quality, String device,
                        Listener listener) {
        this.token = token;
        this.width = width;
        this.height = height;
        this.fps = fps;
        this.hfov = hfov;
        this.quality = quality;
        this.device = device;
        this.listener = listener;
        for (int i = 0; i < POOL; i++) freeBuffers.add(new byte[width * height * 3 / 2]);
    }

    /** Tries the hosts in order (blocking; call off the UI thread). */
    public boolean connect(JSONArray hosts, int port) {
        for (int i = 0; i < hosts.length(); i++) {
            String host = hosts.optString(i);
            try {
                Socket s = new Socket();
                s.connect(new InetSocketAddress(host, port), 2000);
                s.setTcpNoDelay(true);
                s.setSendBufferSize(512 * 1024);
                socket = s;
                out = new DataOutputStream(new BufferedOutputStream(s.getOutputStream(), 256 * 1024));
                JSONObject hello = new JSONObject();
                hello.put("hello", "pocketeye-cam");
                hello.put("token", token);
                hello.put("width", width);
                hello.put("height", height);
                hello.put("fps", fps);
                hello.put("hfov", hfov);
                hello.put("device", device);
                synchronized (this) {
                    out.write((hello.toString() + "\n").getBytes("UTF-8"));
                    out.flush();
                }
                running = true;
                start(host + ":" + port);
                return true;
            } catch (Exception e) {
                // try the next address
            }
        }
        return false;
    }

    private void start(final String laptop) {
        for (int i = 0; i < 2; i++) {
            Thread t = new Thread(new Runnable() {
                @Override
                public void run() {
                    encodeLoop();
                }
            }, "jpeg-" + i);
            t.setPriority(Thread.MAX_PRIORITY - 1);
            t.start();
        }
        new Thread(new Runnable() {
            @Override
            public void run() {
                sendLoop();
            }
        }, "sender").start();
        new Thread(new Runnable() {
            @Override
            public void run() {
                receiveLoop(laptop);
            }
        }, "receiver").start();
    }

    /** Called from the camera callback; copies the frame or drops it if busy. */
    public void offerFrame(byte[] nv21, long timeUs) {
        if (!running) return;
        byte[] buf;
        synchronized (freeBuffers) {
            buf = freeBuffers.poll();
        }
        if (buf == null || nv21.length < buf.length) {
            framesDropped++;
            return;
        }
        System.arraycopy(nv21, 0, buf, 0, buf.length);
        synchronized (jobs) {
            jobs.add(new Job(nextSeqIn++, timeUs, buf));
            jobs.notify();
        }
    }

    private void encodeLoop() {
        ByteArrayOutputStream bos = new ByteArrayOutputStream(256 * 1024);
        Rect rect = new Rect(0, 0, width, height);
        while (running) {
            Job job;
            synchronized (jobs) {
                while (running && jobs.isEmpty()) {
                    try {
                        jobs.wait(200);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
                job = jobs.poll();
            }
            if (job == null) continue;
            byte[] jpeg = null;
            try {
                bos.reset();
                if (rotate180) rotate180(job.nv21, width, height);
                YuvImage img = new YuvImage(job.nv21, android.graphics.ImageFormat.NV21, width, height, null);
                if (img.compressToJpeg(rect, quality, bos)) jpeg = bos.toByteArray();
            } catch (Exception e) {
                jpeg = null;
            }
            synchronized (freeBuffers) {
                freeBuffers.add(job.nv21);
            }
            synchronized (encoded) {
                encoded.put(job.seq, new Encoded(job.timeUs, jpeg));
                encoded.notify();
            }
        }
    }

    private void sendLoop() {
        try {
            while (running) {
                Encoded e;
                synchronized (encoded) {
                    long waitStart = SystemClock.uptimeMillis();
                    while (running && !encoded.containsKey(nextSeqOut)) {
                        // A frame can be stuck in the other encoder: give up after 150 ms.
                        if (!encoded.isEmpty() && SystemClock.uptimeMillis() - waitStart > 150) {
                            nextSeqOut = encoded.firstKey();
                            break;
                        }
                        encoded.wait(20);
                    }
                    if (!running) break;
                    e = encoded.remove(nextSeqOut);
                    nextSeqOut++;
                }
                if (e == null || e.jpeg == null) {
                    framesDropped++;
                    continue;
                }
                synchronized (this) {
                    out.writeInt(0x50454631); // "PEF1"
                    out.writeLong(e.timeUs);
                    out.writeInt(e.jpeg.length);
                    out.write(e.jpeg);
                    out.flush();
                }
                framesSent++;
                bytesSent += e.jpeg.length + 16;
            }
        } catch (Exception ex) {
            fail("send: " + ex.getMessage());
        }
    }

    private void receiveLoop(String laptop) {
        try {
            BufferedReader in = new BufferedReader(new InputStreamReader(socket.getInputStream(), "UTF-8"));
            String line;
            while (running && (line = in.readLine()) != null) {
                JSONObject msg = new JSONObject(line);
                String type = msg.optString("type");
                if ("hello_ok".equals(type)) {
                    listener.onConnected(laptop);
                } else if ("ping".equals(type)) {
                    long laptopUs = msg.getLong("t");
                    long phoneUs = SystemClock.elapsedRealtimeNanos() / 1000;
                    synchronized (this) {
                        out.writeInt(0x50455031); // "PEP1"
                        out.writeLong(laptopUs);
                        out.writeLong(phoneUs);
                        out.flush();
                    }
                } else if ("result".equals(type)) {
                    JSONArray lines = msg.optJSONArray("lines");
                    StringBuilder sb = new StringBuilder();
                    for (int i = 0; lines != null && i < lines.length(); i++) {
                        if (i > 0) sb.append('\n');
                        sb.append(lines.optString(i));
                    }
                    listener.onResult(sb.toString(), msg.optString("decision", ""));
                } else if ("error".equals(type)) {
                    fail(msg.optString("text", "laptop refused the connection"));
                    return;
                }
            }
            fail("laptop closed the connection");
        } catch (Exception ex) {
            fail("receive: " + ex.getMessage());
        }
    }

    private void fail(String reason) {
        if (!running) return;
        running = false;
        close();
        listener.onClosed(reason);
    }

    /** Turn every frame upside down before sending (sensor mounted at 270 degrees). */
    public void setRotate180(boolean on) {
        rotate180 = on;
    }

    /** Rotates an NV21 frame by 180 degrees in place. */
    static void rotate180(byte[] nv21, int width, int height) {
        int n = width * height;
        for (int i = 0, j = n - 1; i < j; i++, j--) {
            byte t = nv21[i];
            nv21[i] = nv21[j];
            nv21[j] = t;
        }
        // Interleaved V/U pairs: reverse the order of the pairs, keep each pair.
        int pairs = n / 4;
        for (int i = 0, j = pairs - 1; i < j; i++, j--) {
            int a = n + 2 * i, b = n + 2 * j;
            byte v = nv21[a], u = nv21[a + 1];
            nv21[a] = nv21[b];
            nv21[a + 1] = nv21[b + 1];
            nv21[b] = v;
            nv21[b + 1] = u;
        }
    }

    public boolean isRunning() {
        return running;
    }

    public void close() {
        running = false;
        synchronized (jobs) {
            jobs.notifyAll();
        }
        synchronized (encoded) {
            encoded.notifyAll();
        }
        try {
            if (socket != null) socket.close();
        } catch (IOException ignored) {
            // closing anyway
        }
    }
}
