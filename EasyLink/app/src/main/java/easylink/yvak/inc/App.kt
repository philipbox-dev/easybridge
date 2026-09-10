package easylink.yvak.inc

import android.app.Application
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.os.LocaleListCompat
import org.osmdroid.config.Configuration
import java.io.File

class App : Application() {
    override fun onCreate() {
        super.onCreate()
        Bridge.init(this)
        Bridge.notif.createChannels()

        // Язык из настроек
        applyLanguage(Bridge.prefs.language.value)

        // osmdroid: кэш тайлов в app-private каталоге
        Configuration.getInstance().apply {
            userAgentValue = "EasyLink/1.0"
            osmdroidBasePath = File(cacheDir, "osm")
            osmdroidTileCache = File(cacheDir, "osm/tiles")
        }
    }

    companion object {
        fun applyLanguage(lang: String) {
            AppCompatDelegate.setApplicationLocales(
                if (lang == "system") LocaleListCompat.getEmptyLocaleList()
                else LocaleListCompat.forLanguageTags(lang))
        }
    }
}
