package easylink.yvak.inc.ui.screens

import android.location.Location
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
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
import org.osmdroid.views.overlay.Marker
import org.osmdroid.views.overlay.Polyline

/** #Ф7 Маршрут до узла: линия я → последняя позиция узла. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun RouteScreen(idHex: String, nav: NavHostController) {
    val id = idHex.removePrefix("0x").toLongOrNull(16) ?: 0L
    val peers by Bridge.location.peers.collectAsState()
    val myLoc by Bridge.location.myLocation.collectAsState()
    val nodes by Bridge.nodes.nodes.collectAsState()
    val peer = peers[id]
    val name = nodes[id]?.displayName ?: idHex

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.route_to_node) + " · " + name) },
                navigationIcon = {
                    IconButton(onClick = { nav.popBackStack() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, null)
                    }
                },
            )
        },
    ) { pad ->
        Box(Modifier.fillMaxSize().padding(pad)) {
            if (peer == null || myLoc == null) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text(stringResource(
                        if (peer == null) R.string.aim_no_node_gps else R.string.aim_no_my_gps))
                }
            } else {
                val me = myLoc!!
                AndroidView(
                    modifier = Modifier.fillMaxSize(),
                    factory = { c ->
                        MapView(c).apply {
                            setTileSource(TileSourceFactory.MAPNIK)
                            setMultiTouchControls(true)
                            zoomController.setVisibility(
                                CustomZoomButtonsController.Visibility.NEVER)
                        }
                    },
                    update = { map ->
                        map.overlays.clear()
                        val a = GeoPoint(me.latitude, me.longitude)
                        val b = GeoPoint(peer.lat, peer.lon)
                        val line = Polyline()
                        line.setPoints(listOf(a, b))
                        line.outlinePaint.color =
                            android.graphics.Color.argb(200, 211, 47, 47)
                        line.outlinePaint.strokeWidth = 8f
                        map.overlays.add(line)
                        val m1 = Marker(map); m1.position = a
                        m1.title = map.context.getString(R.string.map_me)
                        map.overlays.add(m1)
                        val m2 = Marker(map); m2.position = b; m2.title = name
                        map.overlays.add(m2)
                        // вписать обе точки
                        val bb = org.osmdroid.util.BoundingBox.fromGeoPoints(listOf(a, b))
                        map.post { try { map.zoomToBoundingBox(bb, false, 120) } catch (_: Exception) {} }
                    },
                )
                val res = FloatArray(1)
                Location.distanceBetween(me.latitude, me.longitude, peer.lat, peer.lon, res)
                val d = if (res[0] >= 1000) "%.2f км".format(res[0] / 1000)
                else "${res[0].toInt()} м"
                Surface(
                    modifier = Modifier
                        .align(Alignment.TopCenter)
                        .padding(10.dp),
                    shape = MaterialTheme.shapes.large,
                    tonalElevation = 4.dp,
                ) {
                    Text(stringResource(R.string.route_dist_fmt, d),
                        Modifier.padding(horizontal = 14.dp, vertical = 8.dp),
                        style = MaterialTheme.typography.titleSmall)
                }
            }
        }
    }
}
