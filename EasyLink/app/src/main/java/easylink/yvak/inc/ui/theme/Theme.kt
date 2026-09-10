package easylink.yvak.inc.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

// ── Морская палитра ───────────────────────────────────────────
private val SeaLight: ColorScheme = lightColorScheme(
    primary = Color(0xFF0B62B8),
    onPrimary = Color.White,
    primaryContainer = Color(0xFFCDE5FF),
    onPrimaryContainer = Color(0xFF001D33),
    secondary = Color(0xFF0288D1),
    onSecondary = Color.White,
    secondaryContainer = Color(0xFFC7E7FF),
    onSecondaryContainer = Color(0xFF001E2C),
    tertiary = Color(0xFF00838F),
    onTertiary = Color.White,
    tertiaryContainer = Color(0xFFA8EEFF),
    onTertiaryContainer = Color(0xFF001F24),
    error = Color(0xFFBA1A1A),
    onError = Color.White,
    errorContainer = Color(0xFFFFDAD6),
    onErrorContainer = Color(0xFF410002),
    background = Color(0xFFF4F9FF),
    onBackground = Color(0xFF0F1D2A),
    surface = Color(0xFFFBFDFF),
    onSurface = Color(0xFF0F1D2A),
    surfaceVariant = Color(0xFFDDE7F2),
    onSurfaceVariant = Color(0xFF41484F),
    outline = Color(0xFF71787F),
    surfaceContainer = Color(0xFFE9F1FA),
    surfaceContainerHigh = Color(0xFFE1EBF5),
    surfaceContainerHighest = Color(0xFFDAE4EF),
    surfaceContainerLow = Color(0xFFF0F6FD),
)

private val SeaDark: ColorScheme = darkColorScheme(
    primary = Color(0xFF8FCDFF),
    onPrimary = Color(0xFF003353),
    primaryContainer = Color(0xFF004A77),
    onPrimaryContainer = Color(0xFFCDE5FF),
    secondary = Color(0xFF84CFFF),
    onSecondary = Color(0xFF00344A),
    secondaryContainer = Color(0xFF004C69),
    onSecondaryContainer = Color(0xFFC7E7FF),
    tertiary = Color(0xFF4FD8E8),
    onTertiary = Color(0xFF00363D),
    tertiaryContainer = Color(0xFF004F57),
    onTertiaryContainer = Color(0xFFA8EEFF),
    error = Color(0xFFFFB4AB),
    onError = Color(0xFF690005),
    errorContainer = Color(0xFF93000A),
    onErrorContainer = Color(0xFFFFDAD6),
    background = Color(0xFF071A2C),
    onBackground = Color(0xFFD8E4F1),
    surface = Color(0xFF0B2033),
    onSurface = Color(0xFFD8E4F1),
    surfaceVariant = Color(0xFF243545),
    onSurfaceVariant = Color(0xFFC1C9D1),
    outline = Color(0xFF8B939B),
    surfaceContainer = Color(0xFF10283D),
    surfaceContainerHigh = Color(0xFF163049),
    surfaceContainerHighest = Color(0xFF1C3853),
    surfaceContainerLow = Color(0xFF0C2236),
)

@Composable
fun EasyLinkTheme(
    themePref: String,   // system | light | dark
    content: @Composable () -> Unit,
) {
    val dark = when (themePref) {
        "light" -> false
        "dark" -> true
        else -> isSystemInDarkTheme()
    }
    MaterialTheme(
        colorScheme = if (dark) SeaDark else SeaLight,
        content = content,
    )
}
