package com.fastcam.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable

private val DarkColorScheme = darkColorScheme(
    primary = AmberAccent,
    secondary = GrayText,
    background = DarkGray,
    surface = SurfaceGray,
    onPrimary = DarkGray,
    onBackground = White,
    onSurface = White
)

@Composable
fun FastCamTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = DarkColorScheme,
        typography = Typography,
        content = content
    )
}
