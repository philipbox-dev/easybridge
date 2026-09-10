package easylink.yvak.inc.ui.screens

import android.os.BatteryManager
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.navigation.NavHostController
import easylink.yvak.inc.Bridge
import easylink.yvak.inc.R

/** #У6 Батареи всей группы: телефон, своё устройство, узлы. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun BatteriesScreen(nav: NavHostController) {
    val diag by Bridge.device.diag.collectAsState()
    val nodes by Bridge.nodes.nodes.collectAsState()
    val ctx = LocalContext.current
    val phoneBatt = remember {
        try {
            (ctx.getSystemService(android.content.Context.BATTERY_SERVICE) as BatteryManager)
                .getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY)
        } catch (_: Exception) { -1 }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.batteries_title)) },
                navigationIcon = {
                    IconButton(onClick = { nav.popBackStack() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, null)
                    }
                },
            )
        },
    ) { pad ->
        Column(
            Modifier
                .fillMaxSize()
                .padding(pad)
                .padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            if (phoneBatt in 0..100) {
                BattRow("📱 " + stringResource(R.string.tab_device), phoneBatt)
            }
            if (diag.batt in 0..100) {
                BattRow("📡 " + stringResource(R.string.batt_my_device), diag.batt)
            }
            for (n in nodes.values.sortedBy { it.presence }) {
                BattRow(n.displayName, if (n.batt in 0..100) n.batt else -1)
            }
        }
    }
}

@Composable
private fun BattRow(name: String, percent: Int) {
    Card {
        Row(
            Modifier
                .fillMaxWidth()
                .padding(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(name, Modifier.weight(1f), fontWeight = FontWeight.Bold)
            if (percent < 0) {
                Text(stringResource(R.string.batt_unknown),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                LinearProgressIndicator(
                    progress = { percent / 100f },
                    modifier = Modifier.width(110.dp),
                    color = when {
                        percent > 50 -> Color(0xFF4CAF50)
                        percent > 20 -> Color(0xFFFFA726)
                        else -> MaterialTheme.colorScheme.error
                    },
                )
                Spacer(Modifier.width(10.dp))
                Text("$percent%", fontWeight = FontWeight.Bold)
            }
        }
    }
}
