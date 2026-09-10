package easylink.yvak.inc.ui.screens

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material.icons.filled.Router
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.navigation.NavHostController
import easylink.yvak.inc.Bridge
import easylink.yvak.inc.R

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ScannerScreen(nav: NavHostController) {
    val results by Bridge.ble.scanResults.collectAsState()
    val scanning by Bridge.ble.scanning.collectAsState()
    var showAll by remember { mutableStateOf(false) }

    DisposableEffect(Unit) {
        Bridge.ble.startScan()
        onDispose { Bridge.ble.stopScan() }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.scan_title)) },
                navigationIcon = {
                    IconButton(onClick = { nav.popBackStack() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, null)
                    }
                },
                actions = { if (scanning) CircularProgressIndicator(Modifier.size(24.dp)) },
            )
        },
    ) { pad ->
        val shown = if (showAll) results else results.filter { it.isBridge }
        Column(
            Modifier
                .fillMaxSize()
                .padding(pad)
                .padding(horizontal = 12.dp),
        ) {
            // ── Антенна! Каждый раз перед подключением ──
            Card(
                colors = androidx.compose.material3.CardDefaults.cardColors(
                    containerColor = MaterialTheme.colorScheme.errorContainer),
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(
                    stringResource(R.string.antenna_warn_short),
                    Modifier.padding(10.dp),
                    style = MaterialTheme.typography.labelLarge,
                    fontWeight = FontWeight.Bold,
                    color = MaterialTheme.colorScheme.onErrorContainer,
                )
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(stringResource(R.string.scan_show_all), Modifier.weight(1f))
                Switch(checked = showAll, onCheckedChange = { showAll = it })
            }
            if (shown.isEmpty()) {
                Text(
                    stringResource(if (scanning) R.string.scan_hint else R.string.scan_empty),
                    Modifier.padding(vertical = 24.dp),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            LazyColumn(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                items(shown, key = { it.addr }) { d ->
                    Card(
                        Modifier
                            .fillMaxWidth()
                            .clickable {
                                Bridge.connect(d.addr, d.name)
                                nav.popBackStack()
                            }) {
                        Row(
                            Modifier.padding(14.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Icon(
                                if (d.isBridge) Icons.Filled.Router else Icons.Filled.Bluetooth,
                                null,
                                tint = if (d.isBridge) MaterialTheme.colorScheme.primary
                                else MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                            Spacer(Modifier.width(12.dp))
                            Column(Modifier.weight(1f)) {
                                Text(d.name, fontWeight = FontWeight.Bold)
                                Text(d.addr, style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                            Text("${d.rssi} dBm",
                                style = MaterialTheme.typography.labelMedium)
                        }
                    }
                }
            }
        }
    }
}
