package easylink.yvak.inc.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
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
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Groups
import androidx.compose.material.icons.filled.Person
import androidx.compose.material.icons.filled.Public
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Badge
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.navigation.NavHostController
import easylink.yvak.inc.Bridge
import easylink.yvak.inc.R
import easylink.yvak.inc.proto.MsgDir
import easylink.yvak.inc.proto.MsgKind

@Composable
fun ChatListScreen(nav: NavHostController) {
    val chats by Bridge.chat.chats.collectAsState()
    var showCreate by remember { mutableStateOf(false) }
    var showNewGroup by remember { mutableStateOf(false) }
    var showNewDm by remember { mutableStateOf(false) }

    Scaffold(
        floatingActionButton = {
            FloatingActionButton(onClick = { showCreate = true }) {
                Icon(Icons.Filled.Add, null)
            }
        },
    ) { pad ->
        LazyColumn(
            Modifier
                .fillMaxSize()
                .padding(pad)
                .padding(horizontal = 12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            items(chats, key = { it.key }) { chat ->
                val peerNode = if (chat.isDm)
                    Bridge.nodes.nodes.collectAsState().value[chat.peerId] else null
                Card(
                    Modifier
                        .fillMaxWidth()
                        .clickable { nav.navigate("chat/${chat.key}") }) {
                    Row(
                        Modifier.padding(12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Box(
                            Modifier.size(44.dp),
                            contentAlignment = Alignment.Center,
                        ) {
                            Icon(
                                when {
                                    chat.isDm -> Icons.Filled.Person
                                    chat.isGroup -> Icons.Filled.Groups
                                    else -> Icons.Filled.Public
                                },
                                null,
                                tint = MaterialTheme.colorScheme.primary,
                            )
                            if (chat.isDm) {
                                Box(
                                    Modifier
                                        .align(Alignment.BottomEnd)
                                        .size(10.dp)
                                        .clip(CircleShape)
                                        .background(presenceColor(peerNode?.presence ?: 2)))
                            }
                        }
                        Spacer(Modifier.width(10.dp))
                        Column(Modifier.weight(1f)) {
                            Text(
                                when {
                                    chat.isDm -> chat.title.ifEmpty {
                                        peerNode?.displayName ?: "0x%08x".format(chat.peerId)
                                    }
                                    chat.isGroup -> chat.title
                                    else -> stringResource(R.string.chat_general)
                                },
                                fontWeight = FontWeight.Bold,
                            )
                            val last = chat.lastMessage
                            val preview = when {
                                last == null -> stringResource(
                                    if (chat.isGroup || chat.isDm) R.string.chat_empty
                                    else R.string.chat_general_desc)
                                last.kind == MsgKind.VOICE ->
                                    "🎤 " + stringResource(R.string.voice_message)
                                last.kind == MsgKind.IMAGE ->
                                    "🖼 " + stringResource(R.string.image_message)
                                last.kind == MsgKind.SYSTEM -> last.text
                                else -> {
                                    val who = if (last.dir == MsgDir.OUT)
                                        stringResource(R.string.me_label)
                                    else last.nodeName
                                    if (who.isEmpty()) last.text else "$who: ${last.text}"
                                }
                            }
                            Text(
                                preview,
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                        }
                        if (chat.unread > 0) {
                            Badge { Text("${chat.unread}") }
                        }
                    }
                }
            }
        }
    }

    // ── Что создаём? ──
    if (showCreate) {
        AlertDialog(
            onDismissRequest = { showCreate = false },
            title = { Text(stringResource(R.string.chat_new_group) + " / " +
                stringResource(R.string.new_dm)) },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = { showCreate = false; showNewDm = true },
                        modifier = Modifier.fillMaxWidth(),
                    ) { Text(stringResource(R.string.new_dm)) }
                    Button(
                        onClick = { showCreate = false; showNewGroup = true },
                        modifier = Modifier.fillMaxWidth(),
                    ) { Text(stringResource(R.string.chat_new_group)) }
                }
            },
            confirmButton = {
                TextButton(onClick = { showCreate = false }) {
                    Text(stringResource(R.string.cancel))
                }
            },
        )
    }

    if (showNewDm) {
        val nodes by Bridge.nodes.nodes.collectAsState()
        AlertDialog(
            onDismissRequest = { showNewDm = false },
            title = { Text(stringResource(R.string.new_dm_pick)) },
            text = {
                Column {
                    if (nodes.isEmpty()) Text(stringResource(R.string.nodes_empty))
                    for (n in nodes.values.sortedBy { it.presence }) {
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .clickable {
                                    val key = Bridge.chat.createDm(n.id, n.displayName)
                                    showNewDm = false
                                    nav.navigate("chat/$key")
                                }
                                .padding(vertical = 10.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Box(
                                Modifier
                                    .size(8.dp)
                                    .clip(CircleShape)
                                    .background(presenceColor(n.presence)))
                            Spacer(Modifier.width(8.dp))
                            Text(n.displayName)
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { showNewDm = false }) {
                    Text(stringResource(R.string.cancel))
                }
            },
        )
    }

    if (showNewGroup) {
        var name by remember { mutableStateOf("") }
        AlertDialog(
            onDismissRequest = { showNewGroup = false },
            title = { Text(stringResource(R.string.chat_new_group)) },
            text = {
                OutlinedTextField(
                    value = name, onValueChange = { name = it },
                    label = { Text(stringResource(R.string.group_name_hint)) },
                    singleLine = true,
                )
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        if (name.isNotBlank()) {
                            val g = Bridge.chat.createGroup(name.trim())
                            showNewGroup = false
                            nav.navigate("chat/${g.chatKey}")
                        }
                    },
                ) { Text(stringResource(R.string.group_create)) }
            },
            dismissButton = {
                TextButton(onClick = { showNewGroup = false }) {
                    Text(stringResource(R.string.cancel))
                }
            },
        )
    }
}
