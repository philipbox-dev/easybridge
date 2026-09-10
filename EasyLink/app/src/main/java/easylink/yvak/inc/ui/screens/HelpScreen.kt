package easylink.yvak.inc.ui.screens

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.navigation.NavHostController
import easylink.yvak.inc.R

/** Справка: как работает система, LoRa, устройства, советы. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HelpScreen(nav: NavHostController) {
    val sections = listOf(
        R.string.help_s1_t to R.string.help_s1_b,
        R.string.help_s2_t to R.string.help_s2_b,
        R.string.help_s3_t to R.string.help_s3_b,
        R.string.help_s4_t to R.string.help_s4_b,
        R.string.help_s5_t to R.string.help_s5_b,
        R.string.help_s6_t to R.string.help_s6_b,
        R.string.help_s7_t to R.string.help_s7_b,
    )

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.help_title)) },
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
                .verticalScroll(rememberScrollState())
                .padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            for ((i, s) in sections.withIndex()) {
                var expanded by remember { mutableStateOf(i == 0) }
                Card(Modifier.fillMaxWidth()) {
                    Column(
                        Modifier
                            .clickable { expanded = !expanded }
                            .padding(14.dp),
                    ) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(
                                stringResource(s.first),
                                style = MaterialTheme.typography.titleMedium,
                                fontWeight = FontWeight.Bold,
                                modifier = Modifier.weight(1f),
                            )
                            Icon(
                                if (expanded) Icons.Filled.ExpandLess
                                else Icons.Filled.ExpandMore,
                                null,
                            )
                        }
                        AnimatedVisibility(expanded) {
                            Text(
                                stringResource(s.second),
                                style = MaterialTheme.typography.bodyMedium,
                                modifier = Modifier.padding(top = 8.dp),
                            )
                        }
                    }
                }
            }
        }
    }
}
