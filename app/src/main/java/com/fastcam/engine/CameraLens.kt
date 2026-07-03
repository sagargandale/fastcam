package com.fastcam.engine

data class CameraLens(
    val id: String,
    val focalLength: Float,
    val facing: Int // 0 = front, 1 = back, 2 = external
)
