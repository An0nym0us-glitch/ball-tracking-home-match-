package com.pocketeye.camera;

import com.google.zxing.BarcodeFormat;
import com.google.zxing.BinaryBitmap;
import com.google.zxing.DecodeHintType;
import com.google.zxing.PlanarYUVLuminanceSource;
import com.google.zxing.Result;
import com.google.zxing.common.HybridBinarizer;
import com.google.zxing.qrcode.QRCodeReader;

import java.util.Collections;
import java.util.EnumMap;
import java.util.Map;

/** Reads the laptop's pairing QR code from a camera preview frame (NV21). */
final class QrDecoder {
    private final QRCodeReader reader = new QRCodeReader();
    private final Map<DecodeHintType, Object> hints = new EnumMap<>(DecodeHintType.class);

    QrDecoder() {
        hints.put(DecodeHintType.TRY_HARDER, Boolean.TRUE);
        hints.put(DecodeHintType.POSSIBLE_FORMATS, Collections.singletonList(BarcodeFormat.QR_CODE));
    }

    /** Returns the QR text, or null if none was found. */
    String decode(byte[] nv21, int width, int height) {
        try {
            PlanarYUVLuminanceSource src = new PlanarYUVLuminanceSource(nv21, width, height, 0, 0, width, height, false);
            Result r = reader.decode(new BinaryBitmap(new HybridBinarizer(src)), hints);
            return r.getText();
        } catch (Exception e) {
            return null;
        } finally {
            reader.reset();
        }
    }
}
