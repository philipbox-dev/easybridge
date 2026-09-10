package easylink.yvak.inc.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.navigation.NavHostController
import easylink.yvak.inc.Bridge
import easylink.yvak.inc.R
import org.osmdroid.tileprovider.tilesource.TileSourceFactory
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.CustomZoomButtonsController
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.Polyline

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun TrackScreen(nav: NavHostController) {
    val points by Bridge.track.points.collectAsState()
    val dist by Bridge.track.distanceM.collectAsState()
    val ctx = LocalContext.current

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.track_title)) },
                navigationIcon = {
                    IconButton(onClick = { nav.popBackStack() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, null)
                    }
                },
            )
        },
    ) { pad ->
        Column(Modifier.fillMaxSize().padding(pad)) {
            Card(Modifier.fillMaxWidth().padding(12.dp)) {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    val km = dist / 1000.0
                    Text(
                        stringResource(R.string.track_stats_fmt, points.size,
                            if (km >= 1) stringResource(R.string.dist_km2_fmt, km)
                            else stringResource(R.string.dist_m_fmt, dist.toInt())),
                        style = MaterialTheme.typography.titleMedium)
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedButton(onClick = {
                            Bridge.track.exportGpx()?.let {
                                shareFile(ctx, it, "application/gpx+xml")
                            }
                        }, enabled = points.isNotEmpty()) {
                            Icon(Icons.Filled.Share, null)
                            Spacer(Modifier.width(6.dp))
                            Text(stringResource(R.string.track_share_gpx))
                        }
                        Button(
                            onClick = { Bridge.track.clear() },
                            colors = ButtonDefaults.buttonColors(
                                containerColor = MaterialTheme.colorScheme.error),
                            enabled = points.isNotEmpty(),
                        ) {
                            Icon(Icons.Filled.Delete, null)
                            Spacer(Modifier.width(6.dp))
                            Text(stringResource(R.string.track_clear))
                        }
                    }
                }
            }

            if (points.isEmpty()) {
                Text(stringResource(R.string.track_empty),
                    Modifier.padding(16.dp),
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                AndroidView(
                    modifier = Modifier.fillMaxSize(),
                    factory = { c ->
                        MapView(c).apply {
                            setTileSource(TileSourceFactory.MAPNIK)
                            setMultiTouchControls(true)
                            zoomController.setVisibility(CustomZoomButtonsController.Visibility.NEVER)
                            controller.setZoom(15.0)
                        }
                    },
                    update = { map ->
                        map.overlays.clear()
                        val line = Polyline()
                        line.setPoints(points.map { GeoPoint(it.lat, it.lon) })
                        map.overlays.add(line)
                        points.lastOrNull()?.let {
                            map.controller.setCenter(GeoPoint(it.lat, it.lon))
                        }
                        map.invalidate()
                    },
                )
            }
        }
    }
}
