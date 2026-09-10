package easylink.yvak.inc.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material.icons.filled.AddLocationAlt
import androidx.compose.material.icons.filled.Bolt
import androidx.compose.material.icons.filled.Call
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Done
import androidx.compose.material.icons.filled.DoneAll
import androidx.compose.material.icons.filled.ErrorOutline
import androidx.compose.material.icons.filled.GroupAdd
import androidx.compose.material.icons.filled.Image
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.LockOpen
import androidx.compose.material.icons.filled.Logout
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Schedule
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
// (QR группы использует Bridge.crypto + QrCodec)
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.navigation.NavHostController
import easylink.yvak.inc.Bridge
import easylink.yvak.inc.R
import easylink.yvak.inc.proto.CHAT_GENERAL
import easylink.yvak.inc.proto.ChatMessage
import easylink.yvak.inc.proto.MsgDir
import easylink.yvak.inc.proto.MsgKind
import easylink.yvak.inc.proto.MsgStatus
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ChatScreen(chatKey: String, nav: NavHostController) {
    val messages by Bridge.chat.messagesFlow(chatKey).collectAsState()
    val chats by Bridge.chat.chats.collectAsState()
    val fontScale by Bridge.prefs.fontScale.flow.collectAsState()
    val chatInfo = chats.firstOrNull { it.key == chatKey }
    val listState = rememberLazyListState()
    var showQuick by remember { mutableStateOf(false) }
    var showInvite by remember { mutableStateOf(false) }
    var showClear by remember { mutableStateOf(false) }

    DisposableEffect(chatKey) {
        Bridge.chat.openChat(chatKey)
        onDispose { Bridge.chat.closeChat() }
    }

    LaunchedEffect(messages.size) {
        if (messages.isNotEmpty()) listState.animateScrollToItem(messages.size - 1)
    }

    var showGroupInfo by remember { mutableStateOf(false) }
    var showKey by remember { mutableStateOf(false) }
    var searchMode by remember { mutableStateOf(false) }
    var searchQuery by remember { mutableStateOf("") }
    var menuFor by remember { mutableStateOf<ChatMessage?>(null) }
    var showUnpin by remember { mutableStateOf(false) }
    val allNodes by Bridge.nodes.nodes.collectAsState()
    val pinnedMap by Bridge.chat.pinned.collectAsState()
    val peerNode = if (chatInfo?.isDm == true) allNodes[chatInfo.peerId] else null

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    if (searchMode) {
                        OutlinedTextField(
                            value = searchQuery,
                            onValueChange = { searchQuery = it },
                            placeholder = { Text(stringResource(R.string.search_msgs)) },
                            singleLine = true,
                            modifier = Modifier.fillMaxWidth(),
                        )
                    } else
                    Column(
                        Modifier.clickable(enabled = chatInfo?.isGroup == true) {
                            showGroupInfo = true
                        },
                    ) {
                        Text(
                            when {
                                chatInfo?.isDm == true -> chatInfo.title.ifEmpty {
                                    peerNode?.displayName ?: "0x%08x".format(chatInfo.peerId)
                                }
                                chatInfo?.isGroup == true -> chatInfo.title
                                else -> stringResource(R.string.chat_general)
                            })
                        val subtitle = when {
                            chatInfo?.isDm == true -> stringResource(when (peerNode?.presence) {
                                0 -> R.string.node_status_online
                                1 -> R.string.node_status_stale
                                else -> R.string.node_status_offline
                            })
                            chatInfo?.isGroup == true -> stringResource(R.string.group_info)
                            else -> stringResource(R.string.online_count_fmt,
                                allNodes.values.count { it.isOnline })
                        }
                        Text(
                            subtitle,
                            style = MaterialTheme.typography.labelSmall,
                            color = if (chatInfo?.isDm == true && peerNode?.isOnline == true)
                                MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                },
                navigationIcon = {
                    IconButton(onClick = { nav.popBackStack() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, null)
                    }
                },
                actions = {
                    IconButton(onClick = {
                        searchMode = !searchMode
                        if (!searchMode) searchQuery = ""
                    }) {
                        Icon(
                            if (searchMode) Icons.Filled.Close else Icons.Filled.Search,
                            stringResource(R.string.search_msgs))
                    }
                    if (chatInfo?.isDm == true) {
                        IconButton(onClick = {
                            PttUi.requestCallTo(chatInfo.peerId,
                                chatInfo.title.ifEmpty { peerNode?.displayName ?: "" })
                        }) {
                            Icon(Icons.Filled.Call, stringResource(R.string.call_action))
                        }
                    }
                    if (chatInfo?.isGroup == true) {
                        IconButton(onClick = { showInvite = true }) {
                            Icon(Icons.Filled.GroupAdd, stringResource(R.string.group_invite))
                        }
                    }
                    val hasKey by Bridge.crypto.keys.collectAsState()
                    IconButton(onClick = { showKey = true }) {
                        Icon(
                            if (hasKey.containsKey(chatKey)) Icons.Filled.Lock
                            else Icons.Filled.LockOpen,
                            stringResource(R.string.chat_key_title),
                            tint = if (hasKey.containsKey(chatKey))
                                MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    IconButton(onClick = { showClear = true }) {
                        Icon(Icons.Filled.Delete, stringResource(R.string.delete_history))
                    }
                },
            )
        },
    ) { pad ->
        Column(
            Modifier
                .fillMaxSize()
                .padding(pad)
                .imePadding(),
        ) {
            // ── 📌 Закреплённое ──
            pinnedMap[chatKey]?.let { pinnedText ->
                Card(
                    Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 10.dp, vertical = 2.dp)
                        .clickable { showUnpin = true },
                ) {
                    Row(Modifier.padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text(stringResource(R.string.pinned_label),
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.primary)
                        Spacer(Modifier.width(8.dp))
                        Text(pinnedText, maxLines = 1,
                            overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis,
                            style = MaterialTheme.typography.bodySmall)
                    }
                }
            }

            val shownMessages = if (searchQuery.isBlank()) messages
            else messages.filter {
                it.text.contains(searchQuery, ignoreCase = true) ||
                    it.nodeName.contains(searchQuery, ignoreCase = true)
            }
            LazyColumn(
                Modifier
                    .weight(1f)
                    .fillMaxWidth()
                    .padding(horizontal = 10.dp),
                state = listState,
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                items(shownMessages, key = { it.id }) { m ->
                    MessageBubble(m, fontScale, onLongPress = { menuFor = it })
                }
            }

            LinkQualityBanner()

            ImageTxBanner()

            if (showQuick) {
                QuickPhrasesRow { phrase ->
                    Bridge.chat.sendText(chatKey, phrase)
                    showQuick = false
                }
            }

            InputRow(
                chatKey = chatKey,
                onToggleQuick = { showQuick = !showQuick },
            )
        }
    }

    // ── Меню сообщения (long-press): закрепить / копировать ──
    menuFor?.let { m ->
        val clipboard = androidx.compose.ui.platform.LocalClipboardManager.current
        AlertDialog(
            onDismissRequest = { menuFor = null },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(m.text.take(200), style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    Bridge.chat.pin(chatKey, m.text)
                    menuFor = null
                }) { Text(stringResource(R.string.pin_msg)) }
            },
            dismissButton = {
                TextButton(onClick = {
                    clipboard.setText(androidx.compose.ui.text.AnnotatedString(m.text))
                    menuFor = null
                }) { Text(stringResource(R.string.copy_msg)) }
            },
        )
    }
    if (showUnpin) {
        AlertDialog(
            onDismissRequest = { showUnpin = false },
            title = { Text(stringResource(R.string.pinned_label)) },
            text = { Text(pinnedMap[chatKey] ?: "") },
            confirmButton = {
                TextButton(onClick = {
                    Bridge.chat.pin(chatKey, "")
                    showUnpin = false
                }) { Text(stringResource(R.string.unpin_msg)) }
            },
            dismissButton = {
                TextButton(onClick = { showUnpin = false }) {
                    Text(stringResource(R.string.close))
                }
            },
        )
    }

    if (showKey) ChatKeyDialog(chatKey) { showKey = false }
    if (showInvite) InviteDialog(chatKey) { showInvite = false }
    if (showGroupInfo && chatInfo?.isGroup == true) {
        GroupInfoDialog(
            chatKey = chatKey,
            groupId = chatInfo.groupId,
            title = chatInfo.title,
            onInvite = { showGroupInfo = false; showInvite = true },
            onLeave = {
                showGroupInfo = false
                Bridge.chat.groupOf(chatKey)?.let { Bridge.chat.leaveGroup(it) }
                nav.popBackStack()
            },
            onDismiss = { showGroupInfo = false },
        )
    }
    if (showClear) {
        AlertDialog(
            onDismissRequest = { showClear = false },
            title = { Text(stringResource(R.string.delete_history)) },
            text = { Text(stringResource(R.string.delete_history_confirm)) },
            confirmButton = {
                TextButton(onClick = {
                    Bridge.chat.clearChatHistory(chatKey)
                    showClear = false
                }) { Text(stringResource(R.string.ok)) }
            },
            dismissButton = {
                TextButton(onClick = { showClear = false }) {
                    Text(stringResource(R.string.cancel))
                }
            },
        )
    }
}

@OptIn(androidx.compose.foundation.ExperimentalFoundationApi::class)
@Composable
private fun MessageBubble(m: ChatMessage, fontScale: Float,
                          onLongPress: (ChatMessage) -> Unit = {}) {
    val out = m.dir == MsgDir.OUT
    val time = remember(m.ts) {
        SimpleDateFormat("HH:mm", Locale.getDefault()).format(Date(m.ts))
    }

    if (m.kind == MsgKind.SYSTEM) {
        Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
            Text(
                m.text,
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        return
    }

    Row(
        Modifier.fillMaxWidth(),
        horizontalArrangement = if (out) Arrangement.End else Arrangement.Start,
    ) {
        Column(
            Modifier
                .widthIn(max = 300.dp)
                .clip(
                    RoundedCornerShape(
                        topStart = 14.dp, topEnd = 14.dp,
                        bottomStart = if (out) 14.dp else 4.dp,
                        bottomEnd = if (out) 4.dp else 14.dp))
                .combinedClickable(onClick = {}, onLongClick = { onLongPress(m) })
                .background(
                    when {
                        m.kind == MsgKind.SOS -> MaterialTheme.colorScheme.errorContainer
                        out -> MaterialTheme.colorScheme.primaryContainer
                        else -> MaterialTheme.colorScheme.surfaceContainerHigh
                    })
                .padding(horizontal = 10.dp, vertical = 6.dp),
        ) {
            if (!out && m.nodeName.isNotEmpty()) {
                Text(
                    m.nodeName,
                    style = MaterialTheme.typography.labelSmall,
                    fontWeight = FontWeight.Bold,
                    color = MaterialTheme.colorScheme.primary,
                )
            }

            when (m.kind) {
                MsgKind.VOICE -> VoiceBubbleBody(m)
                MsgKind.IMAGE -> ImageBubbleBody(m)
                else -> Text(m.text, fontSize = (15 * fontScale).sp)
            }

            Row(verticalAlignment = Alignment.CenterVertically) {
                if (!out) {
                    Text(
                        "$time · ${m.rssi} dBm" +
                            if (m.hops > 0) " · ${m.hops} hop" else "",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                } else {
                    Text(
                        time,
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.width(4.dp))
                    val (icon, tint) = when (m.status) {
                        MsgStatus.SENDING -> Icons.Filled.Schedule to
                            MaterialTheme.colorScheme.onSurfaceVariant
                        MsgStatus.SENT -> Icons.Filled.Done to
                            MaterialTheme.colorScheme.onSurfaceVariant
                        MsgStatus.DELIVERED -> Icons.Filled.DoneAll to
                            MaterialTheme.colorScheme.primary
                        MsgStatus.FAILED -> Icons.Filled.ErrorOutline to
                            MaterialTheme.colorScheme.error
                    }
                    Icon(icon, null, Modifier.size(14.dp), tint = tint)
                }
            }
        }
    }
}

@Composable
private fun VoiceBubbleBody(m: ChatMessage) {
    val playing by Bridge.player.playingPath.collectAsState()
    val isPlaying = playing == m.voicePath
    val seconds = remember(m.voicePath) {
        val f = File(m.voicePath)
        if (f.exists()) ((f.length() - 6) / 594f).coerceIn(0f, 60f).toInt() + 1 else 0
    }
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier.clickable { Bridge.player.toggle(m.voicePath) },
    ) {
        Icon(
            if (isPlaying) Icons.Filled.Stop else Icons.Filled.PlayArrow,
            null,
            Modifier.size(34.dp),
            tint = MaterialTheme.colorScheme.primary,
        )
        Spacer(Modifier.width(6.dp))
        Text("${stringResource(R.string.voice_message)} · ${seconds}s")
    }
}

@Composable
private fun ImageBubbleBody(m: ChatMessage) {
    var full by remember { mutableStateOf(false) }
    val bmp = remember(m.voicePath) {
        try {
            android.graphics.BitmapFactory.decodeFile(m.voicePath)?.asImageBitmap()
        } catch (_: Exception) { null }
    }
    if (bmp != null) {
        androidx.compose.foundation.Image(
            bitmap = bmp,
            contentDescription = stringResource(R.string.image_message),
            modifier = Modifier
                .widthIn(max = 260.dp)
                .clip(RoundedCornerShape(8.dp))
                .clickable { full = true },
        )
        if (full) {
            androidx.compose.ui.window.Dialog(onDismissRequest = { full = false }) {
                androidx.compose.foundation.Image(
                    bitmap = bmp,
                    contentDescription = null,
                    modifier = Modifier
                        .fillMaxWidth()
                        .clickable { full = false },
                )
            }
        }
    } else {
        Text("🖼 " + stringResource(R.string.image_message))
    }
}

@Composable
private fun ImageTxBanner() {
    val tx by Bridge.image.txState.collectAsState()
    val incoming by Bridge.image.incomingFrom.collectAsState()
    val text = when {
        incoming != null ->
            stringResource(R.string.image_incoming_fmt, incoming ?: "")
        tx is easylink.yvak.inc.repo.ImageRepo.TxState.Preparing ->
            stringResource(R.string.image_preparing)
        tx is easylink.yvak.inc.repo.ImageRepo.TxState.Sending -> {
            val s = tx as easylink.yvak.inc.repo.ImageRepo.TxState.Sending
            stringResource(R.string.image_sending_fmt, s.pass, s.of)
        }
        tx is easylink.yvak.inc.repo.ImageRepo.TxState.Done ->
            stringResource(R.string.image_sent)
        else -> null
    } ?: return
    Row(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        androidx.compose.material3.LinearProgressIndicator(
            modifier = Modifier.width(80.dp))
        Spacer(Modifier.width(10.dp))
        Text(text, style = MaterialTheme.typography.labelMedium)
    }
}

@Composable
private fun QuickPhrasesRow(onSend: (String) -> Unit) {
    val phrases = listOf(
        stringResource(R.string.qp_yes), stringResource(R.string.qp_no),
        stringResource(R.string.qp_ok), stringResource(R.string.qp_coming),
        stringResource(R.string.qp_help), stringResource(R.string.qp_approach),
        stringResource(R.string.qp_stop), stringResource(R.string.qp_run),
        stringResource(R.string.qp_wait), stringResource(R.string.qp_hello),
    )
    Row(
        Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 8.dp),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        for (p in phrases) {
            AssistChip(onClick = { onSend(p) }, label = { Text(p) })
        }
    }
}

@Composable
private fun InputRow(chatKey: String, onToggleQuick: () -> Unit) {
    var text by remember { mutableStateOf("") }
    val recording by Bridge.recorder.recording.collectAsState()
    val enterToSend by Bridge.prefs.enterToSend.flow.collectAsState()
    val ctx = androidx.compose.ui.platform.LocalContext.current

    val pickImage = androidx.activity.compose.rememberLauncherForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.GetContent()
    ) { uri -> if (uri != null) Bridge.sendImage(chatKey, uri) }

    Row(
        Modifier
            .fillMaxWidth()
            .padding(8.dp),
        verticalAlignment = Alignment.Bottom,
    ) {
        IconButton(onClick = onToggleQuick) {
            Icon(Icons.Filled.Bolt, stringResource(R.string.quick_phrases),
                tint = MaterialTheme.colorScheme.primary)
        }
        IconButton(onClick = { pickImage.launch("image/*") }) {
            Icon(Icons.Filled.Image, stringResource(R.string.attach_photo),
                tint = MaterialTheme.colorScheme.primary)
        }
        IconButton(onClick = {
            val loc = Bridge.location.myLocation.value
            if (loc != null) {
                Bridge.chat.sendText(chatKey, Bridge.location.formatLocText(loc))
            }
        }) {
            Icon(Icons.Filled.AddLocationAlt, stringResource(R.string.send_location),
                tint = MaterialTheme.colorScheme.primary)
        }

        OutlinedTextField(
            value = text,
            onValueChange = { v ->
                if (enterToSend && v.endsWith("\n")) {
                    val t = v.trimEnd('\n')
                    if (t.isNotBlank()) {
                        Bridge.chat.sendText(chatKey, t)
                        text = ""
                    }
                } else text = v
            },
            modifier = Modifier.weight(1f),
            placeholder = {
                Text(
                    if (recording) stringResource(R.string.voice_recording)
                    else stringResource(R.string.msg_hint))
            },
            maxLines = 4,
        )

        if (text.isBlank()) {
            // Микрофон: держать для записи
            IconButton(
                onClick = {},
                modifier = Modifier.pointerInput(chatKey) {
                    detectTapGestures(onPress = {
                        if (Bridge.recorder.start()) {
                            tryAwaitRelease()
                            val res = Bridge.recorder.stop()
                            if (res != null) {
                                val (f, bytes) = res
                                Bridge.chat.sendVoice(chatKey, f.absolutePath, bytes)
                            }
                        }
                    })
                },
            ) {
                Icon(
                    Icons.Filled.Mic, stringResource(R.string.voice_hold_hint),
                    tint = if (recording) MaterialTheme.colorScheme.error
                    else MaterialTheme.colorScheme.primary,
                )
            }
        } else {
            IconButton(onClick = {
                Bridge.chat.sendText(chatKey, text.trim())
                text = ""
            }) {
                Icon(Icons.AutoMirrored.Filled.Send, stringResource(R.string.send),
                    tint = MaterialTheme.colorScheme.primary)
            }
        }
    }
}

@Composable
private fun GroupInfoDialog(
    chatKey: String,
    groupId: Long,
    title: String,
    onInvite: () -> Unit,
    onLeave: () -> Unit,
    onDismiss: () -> Unit,
) {
    val nodes by Bridge.nodes.nodes.collectAsState()
    var members by remember { mutableStateOf<List<Long>>(emptyList()) }
    LaunchedEffect(groupId) {
        members = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
            Bridge.chat.groupMembers(groupId)
        }
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(
                    stringResource(R.string.group_id_label) + ": 0x%08x".format(groupId),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(
                    stringResource(R.string.group_members) +
                        " · " + stringResource(R.string.online_count_fmt,
                        members.count { nodes[it]?.isOnline == true }),
                    style = MaterialTheme.typography.titleSmall,
                )
                if (members.isEmpty()) {
                    Text(stringResource(R.string.group_no_members),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                for (m in members) {
                    val n = nodes[m]
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(n?.displayName ?: "0x%08x".format(m), Modifier.weight(1f))
                        Text(
                            stringResource(when (n?.presence) {
                                0 -> R.string.node_status_online
                                1 -> R.string.node_status_stale
                                else -> R.string.node_status_offline
                            }),
                            style = MaterialTheme.typography.labelSmall,
                            color = presenceColor(n?.presence ?: 2),
                        )
                    }
                }
            }
        },
        confirmButton = {
            Row {
                var showQr by remember { mutableStateOf(false) }
                TextButton(onClick = { showQr = true }) {
                    Text(stringResource(R.string.qr_group))
                }
                if (showQr) {
                    val key = Bridge.crypto.keys.collectAsState().value[chatKey] ?: ""
                    val bmp = remember {
                        easylink.yvak.inc.repo.QrCodec.toBitmap(
                            easylink.yvak.inc.repo.QrCodec.encodeGroup(groupId, title, key))
                    }
                    AlertDialog(
                        onDismissRequest = { showQr = false },
                        title = { Text(stringResource(R.string.qr_group)) },
                        text = {
                            Column {
                                bmp?.let {
                                    androidx.compose.foundation.Image(
                                        bitmap = it.asImageBitmap(),
                                        contentDescription = null,
                                        modifier = Modifier.fillMaxWidth())
                                }
                                Text(stringResource(R.string.qr_group_hint),
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                        },
                        confirmButton = {
                            TextButton(onClick = { showQr = false }) {
                                Text(stringResource(R.string.close))
                            }
                        },
                    )
                }
                TextButton(onClick = onInvite) { Text(stringResource(R.string.group_invite)) }
            }
        },
        dismissButton = {
            TextButton(onClick = onLeave) {
                Text(stringResource(R.string.group_leave),
                    color = MaterialTheme.colorScheme.error)
            }
        },
    )
}

/** #У1 Полоска качества связи над полем ввода. */
@Composable
private fun LinkQualityBanner() {
    val connState by Bridge.ble.connState.collectAsState()
    val nodes by Bridge.nodes.nodes.collectAsState()

    if (connState != easylink.yvak.inc.proto.ConnState.CONNECTED) {
        Row(
            Modifier
                .fillMaxWidth()
                .background(MaterialTheme.colorScheme.errorContainer)
                .padding(horizontal = 12.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(stringResource(R.string.link_ble_off),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onErrorContainer)
        }
        return
    }
    val lastHeard = nodes.values.maxOfOrNull { it.lastSeenMs } ?: 0L
    val age = System.currentTimeMillis() - lastHeard
    val (label, color) = when {
        lastHeard == 0L -> R.string.link_quiet to MaterialTheme.colorScheme.onSurfaceVariant
        age < 60_000 -> R.string.link_good to androidx.compose.ui.graphics.Color(0xFF2E7D32)
        age < 300_000 -> R.string.link_quiet to androidx.compose.ui.graphics.Color(0xFFFFA726)
        else -> R.string.link_stale to MaterialTheme.colorScheme.onSurfaceVariant
    }
    Row(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier
                .size(6.dp)
                .clip(androidx.compose.foundation.shape.CircleShape)
                .background(color))
        Spacer(Modifier.width(6.dp))
        Text(stringResource(label),
            style = MaterialTheme.typography.labelSmall, color = color)
    }
}

@Composable
private fun ChatKeyDialog(chatKey: String, onDismiss: () -> Unit) {
    val keys by Bridge.crypto.keys.collectAsState()
    var pass by remember { mutableStateOf(keys[chatKey] ?: "") }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.chat_key_title)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.chat_key_hint),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                OutlinedTextField(
                    value = pass, onValueChange = { pass = it },
                    label = { Text(stringResource(R.string.psk_field)) },
                    singleLine = true)
                if (keys.containsKey(chatKey)) {
                    Text(stringResource(R.string.chat_key_on),
                        color = MaterialTheme.colorScheme.primary)
                }
            }
        },
        confirmButton = {
            TextButton(onClick = {
                Bridge.crypto.setKey(chatKey, pass.trim()); onDismiss()
            }) { Text(stringResource(R.string.chat_key_set)) }
        },
        dismissButton = {
            TextButton(onClick = {
                Bridge.crypto.setKey(chatKey, ""); onDismiss()
            }) { Text(stringResource(R.string.chat_key_clear)) }
        },
    )
}

@Composable
private fun InviteDialog(chatKey: String, onDismiss: () -> Unit) {
    val nodes by Bridge.nodes.nodes.collectAsState()
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.group_invite)) },
        text = {
            Column {
                if (nodes.isEmpty()) Text(stringResource(R.string.nodes_empty))
                for (n in nodes.values.sortedByDescending { it.lastSeenMs }) {
                    Text(
                        n.displayName,
                        Modifier
                            .fillMaxWidth()
                            .clickable {
                                Bridge.chat.groupOf(chatKey)?.let { g ->
                                    Bridge.chat.inviteToGroup(g, n.id)
                                    Bridge.device.note(
                                        easylink.yvak.inc.repo.UiNote.GroupInvited(n.displayName))
                                }
                                onDismiss()
                            }
                            .padding(vertical = 10.dp),
                    )
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) }
        },
    )
}
