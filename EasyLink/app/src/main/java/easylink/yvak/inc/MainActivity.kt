package easylink.yvak.inc

import android.Manifest
import android.content.Intent
import android.os.Build
import android.os.Bundle
import androidx.activity.compose.setContent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Chat
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Person
import androidx.compose.material.icons.filled.Settings
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import easylink.yvak.inc.repo.UiNote
import easylink.yvak.inc.ui.screens.ChatListScreen
import easylink.yvak.inc.ui.screens.ChatScreen
import easylink.yvak.inc.ui.screens.DebugScreen
import easylink.yvak.inc.ui.screens.DeviceScreen
import easylink.yvak.inc.ui.screens.HelpScreen
import easylink.yvak.inc.ui.screens.HomeScreen
import easylink.yvak.inc.ui.screens.MapScreen
import easylink.yvak.inc.ui.screens.NodeScreen
import easylink.yvak.inc.ui.screens.PttOverlay
import easylink.yvak.inc.ui.screens.ScannerScreen
import easylink.yvak.inc.ui.screens.SettingsScreen
import easylink.yvak.inc.ui.screens.SosOverlay
import easylink.yvak.inc.ui.theme.EasyLinkTheme

class MainActivity : AppCompatActivity() {

    private val openRequest = mutableStateOf<String?>(null)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        openRequest.value = intent?.getStringExtra("open")

        setContent {
            val theme by Bridge.prefs.theme.flow.collectAsState()
            EasyLinkTheme(theme) {
                val accepted by Bridge.prefs.disclaimerAccepted.flow.collectAsState()
                val onboarded by Bridge.prefs.onboardingDone.flow.collectAsState()
                when {
                    !accepted -> easylink.yvak.inc.ui.screens.DisclaimerScreen(
                        onAccept = { Bridge.prefs.disclaimerAccepted.value = true },
                        onDecline = { finish() })
                    !onboarded -> easylink.yvak.inc.ui.screens.OnboardingScreen(
                        onDone = { Bridge.prefs.onboardingDone.value = true })
                    else -> Root(openRequest.value) { openRequest.value = null }
                }
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        openRequest.value = intent.getStringExtra("open")
    }

    override fun onStart() {
        super.onStart()
        Bridge.appVisible = true
        // GPS для карты/радара/прицела, пока приложение на экране
        Bridge.location.startUi(this)
    }

    override fun onStop() {
        Bridge.appVisible = false
        Bridge.location.stopUi(this)
        super.onStop()
    }
}

private data class Tab(val route: String, val icon: @Composable () -> Unit, val labelRes: Int)

@Composable
private fun Root(openRequest: String?, onOpenHandled: () -> Unit) {
    val nav: NavHostController = rememberNavController()
    val snackbar = remember { SnackbarHostState() }

    // ── Разрешения на старте ──
    val permLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()) { }
    LaunchedEffect(Unit) {
        val perms = buildList {
            if (Build.VERSION.SDK_INT >= 31) {
                add(Manifest.permission.BLUETOOTH_SCAN)
                add(Manifest.permission.BLUETOOTH_CONNECT)
            }
            add(Manifest.permission.ACCESS_FINE_LOCATION)
            add(Manifest.permission.RECORD_AUDIO)
            if (Build.VERSION.SDK_INT >= 33) add(Manifest.permission.POST_NOTIFICATIONS)
        }
        permLauncher.launch(perms.toTypedArray())
    }

    // ── Открытие из уведомления ──
    LaunchedEffect(openRequest) {
        when {
            openRequest == null -> {}
            openRequest.startsWith("chat:") -> {
                nav.navigate("chat/${openRequest.removePrefix("chat:")}")
                onOpenHandled()
            }
            else -> onOpenHandled()  // sos/ptt — оверлеи покажутся сами
        }
    }

    // ── Всплывашки от устройства ──
    val ctx = androidx.compose.ui.platform.LocalContext.current
    LaunchedEffect(Unit) {
        Bridge.device.notes.collect { n ->
            val text = when (n) {
                is UiNote.SosSent -> ctx.getString(R.string.sos_sent_fmt, n.count)
                is UiNote.SpeedSwitch -> ctx.getString(R.string.chan_switch_fmt,
                    ctx.getString(if (n.fast) R.string.speed_fast else R.string.speed_slow),
                    n.inSec)
                is UiNote.SpeedApplied -> ctx.getString(R.string.speed_applied_fmt,
                    ctx.getString(if (n.fast) R.string.speed_fast else R.string.speed_slow))
                is UiNote.Psk -> ctx.getString(
                    if (n.enabled) R.string.psk_enabled else R.string.psk_disabled)
                is UiNote.NameSaved -> ctx.getString(R.string.name_saved)
                is UiNote.DeviceError -> ctx.getString(R.string.err_device_fmt, n.desc)
                is UiNote.Ping -> ctx.getString(R.string.dbg_ping_result_fmt, n.ms)
                is UiNote.LocationSent -> ctx.getString(R.string.location_sent)
                is UiNote.GroupInvited -> ctx.getString(R.string.group_invite_sent_fmt, n.nodeName)
                is UiNote.GroupJoined -> ctx.getString(R.string.group_joined_fmt, n.name)
            }
            snackbar.showSnackbar(text)
        }
    }

    val tabs = listOf(
        Tab("home", { Icon(Icons.Filled.Home, null) }, R.string.tab_home),
        Tab("chats", { Icon(Icons.AutoMirrored.Filled.Chat, null) }, R.string.tab_chat),
        Tab("settings", { Icon(Icons.Filled.Settings, null) }, R.string.tab_settings),
        Tab("device", { Icon(Icons.Filled.Person, null) }, R.string.tab_device),
    )
    val backStack by nav.currentBackStackEntryAsState()
    val currentRoute = backStack?.destination?.route
    val showBar = currentRoute in tabs.map { it.route }

    Scaffold(
        snackbarHost = { SnackbarHost(snackbar) },
        bottomBar = {
            if (showBar) NavigationBar {
                for (t in tabs) {
                    NavigationBarItem(
                        selected = currentRoute == t.route,
                        onClick = {
                            nav.navigate(t.route) {
                                popUpTo("home") { saveState = true }
                                launchSingleTop = true
                                restoreState = true
                            }
                        },
                        icon = t.icon,
                        label = { Text(stringResource(t.labelRes)) },
                    )
                }
            }
        },
    ) { pad ->
        NavHost(nav, startDestination = "home", modifier = Modifier.padding(pad)) {
            composable("home") { HomeScreen(nav) }
            composable("chats") { ChatListScreen(nav) }
            composable("chat/{key}") { entry ->
                ChatScreen(entry.arguments?.getString("key") ?: "general", nav)
            }
            composable("settings") { SettingsScreen(nav) }
            composable("device") { DeviceScreen(nav) }
            composable("map") { MapScreen() }
            composable("debug") { DebugScreen() }
            composable("web") { easylink.yvak.inc.ui.screens.WebScreen(nav) }
            composable("scanner") { ScannerScreen(nav) }
            composable("help") { HelpScreen(nav) }
            composable("track") { easylink.yvak.inc.ui.screens.TrackScreen(nav) }
            composable("rangetest") { easylink.yvak.inc.ui.screens.RangeTestScreen(nav) }
            composable("batteries") { easylink.yvak.inc.ui.screens.BatteriesScreen(nav) }
            composable("stats") { easylink.yvak.inc.ui.screens.StatsScreen(nav) }
            composable("route/{id}") { entry ->
                easylink.yvak.inc.ui.screens.RouteScreen(
                    entry.arguments?.getString("id") ?: "0x0", nav)
            }
            composable("node/{id}") { entry ->
                NodeScreen(entry.arguments?.getString("id") ?: "0x0", nav)
            }
        }
    }

    // ── Оверлеи поверх любого экрана ──
    easylink.yvak.inc.ui.screens.PttCallDialogHost()  // выбор адресата/режима звонка
    easylink.yvak.inc.ui.screens.CheckInOverlays()    // «все живы?»
    PttOverlay()
    SosOverlay()
}
