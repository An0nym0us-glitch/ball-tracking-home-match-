package com.pocketeye.pocketeye

import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import kotlin.math.atan

class MainActivity : FlutterActivity() {
    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, "pocketeye/lens").setMethodCallHandler { call, result ->
            if (call.method == "backCameraFov") {
                val w = call.argument<Int>("width") ?: 16
                val h = call.argument<Int>("height") ?: 9
                result.success(backCameraFov(w.toDouble() / h))
            } else {
                result.notImplemented()
            }
        }
    }

    // Horizontal field of view (degrees) of the main back camera for a stream
    // with the given aspect ratio, from the lens focal length and the sensor
    // size. Along the pitch every distance scales with it, so the pitch length
    // can only be measured from the picture as well as this is known.
    private fun backCameraFov(aspect: Double): Map<String, Double>? {
        return try {
            val cm = getSystemService(Context.CAMERA_SERVICE) as CameraManager
            for (id in cm.cameraIdList) {
                val c = cm.getCameraCharacteristics(id)
                if (c.get(CameraCharacteristics.LENS_FACING) != CameraCharacteristics.LENS_FACING_BACK) continue
                val focal = c.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)?.firstOrNull() ?: continue
                val size = c.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE) ?: continue
                val pixels = c.get(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE)
                val active = c.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE)
                var sw = size.width.toDouble()
                var sh = size.height.toDouble()
                if (pixels != null && active != null && pixels.width > 0 && pixels.height > 0) {
                    sw *= active.width().toDouble() / pixels.width
                    sh *= active.height().toDouble() / pixels.height
                }
                // A stream wider than the sensor is cropped top and bottom (full
                // width); a narrower one is cropped at the sides.
                val width = if (aspect >= sw / sh) sw else sh * aspect
                val hfov = Math.toDegrees(2.0 * atan(width / (2.0 * focal)))
                return mapOf("hfov_deg" to hfov, "sigma_deg" to 1.5)
            }
            null
        } catch (e: Exception) {
            null
        }
    }
}
