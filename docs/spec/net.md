# Multiplayer network protocol (patch 1.3)

Scope: the wire format that a patch 1.3 multiplayer client and server exchange over UDP: message bit-streams, the
Huffman coder, the netchan (sequencing, fragmentation, qport), connectionless (out-of-band) packets, the protocol
number, and the structure of the delta coders for user commands and entity / player state.

Sources: VAs in the 1.3 Linux dedicated server (written as `0x80…`) and VAs in the patch 1.3 MP executable (the
client, written as `MP 0x4…`). Function names are descriptive labels chosen for this document. Behaviour marked
"observed" was checked end to end against a running 1.3 dedicated server with `tools/iw2netclient` (section 9). Statements without an address are marked `TODO(spec)`.

openiw2 decision: our server runs without CD-key authorisation (LAN / open mode). It never contacts an authorize
server, never sends heartbeats to a master by default, and does not implement PunkBuster. Where the original does
something else, both behaviours are described below.

## 1. Byte order and basic numbers

- All multi-byte integers on the wire are little-endian. The short / long helpers used by the message writer and
  reader (0x806b1b8, 0x806b1cc) are identity functions on the i386 build.
- Protocol version: **118** (0x76). The connect handler (0x808e2aa) compares the userinfo key `protocol` against
  0x76 at 0x808e346, and the info reply (0x809537c) advertises the same constant. The human-readable version string
  sent with the rejection is `1.3` (0x8149eb7).
- Default UDP port: 28960 (`net_port` cvar default 0x7120, registered at 0x80d59bb).

## 2. Message buffer (msg)

A message is a byte buffer with six pieces of state (0x8067bd8 initialises it, 0x8067c1c resets reading):
`overflowed` flag, `data` pointer, `maxsize`, `cursize` (bytes written), `readcount` (bytes consumed) and `bit`
(a bit cursor counted in bits from the start of `data`).

The format is **mixed**: whole bytes and loose bits live in the same byte stream.

- Whole values (byte 0x806800c, short 0x806809a, long 0x80680ea, 64-bit 0x8068138, raw data 0x8068044) are appended
  at `cursize`. A write that does not fit sets `overflowed` and writes nothing.
- Bits (0x8067c62 for N bits, 0x8067d28 for a zero bit, 0x8067d86 for a one bit) are packed least significant bit
  first into a "bit byte". When `bit` is at a multiple of 8, a fresh byte is appended at `cursize` (initialised to 0)
  and `bit` is moved to the start of that byte; otherwise the next bit goes into the bit byte that was started
  earlier, even if whole bytes were appended after it. The value is written least significant bit first.
- The N-bit writer refuses (sets `overflowed`) when fewer than 4 bytes are free, whatever N is. The single-bit
  writers overflow only when the buffer is completely full.
- Reading mirrors writing: whole values are taken at `readcount`; a bit read at a multiple of 8 takes the byte at
  `readcount` as the new bit byte and advances `readcount`. Reading past `cursize` sets `overflowed` and returns -1
  (bits, byte, short, long; 0x8067e20, 0x8067ece, 0x80683a8, 0x80683f2, 0x8068450), 0 for 64-bit (0x80684ac) and fills
  the destination with 0xFF for raw data (0x80686ac). Shorts are sign-extended.
- Bit length of a written message: `cursize * 8 - ((8 - bit) & 7)` (0x8067c3e).
- Angles: an 8-bit angle is `round(a * 256 / 360) & 255` (0x806831c); a 16-bit angle is `round(a * 65536 / 360)`
  (0x8068366), read back as `value * 360 / 65536` (0x8068688).

Strings (0x80681a4, readers 0x8068522, 0x806858c, 0x8068606):
- Written as the bytes followed by a zero byte. Strings of 1024 characters or more are replaced by an empty string
  with a warning. On write and on read the byte 0x92 is replaced by an apostrophe (0x80b7edc). A second writer
  (0x8068256) does the same with an 8192-character limit.
- The plain reader stops at a zero byte or at the end of data (1024-character buffer). The "big" reader (8192) and the
  "line" reader (1024) also turn `%` into `.`; the line reader additionally stops at a newline.

## 3. Huffman coder

The game messages (not the connectionless ones) are compressed with a Huffman code over bytes.

### Tree

- The tree is built at start-up (0x806b12c) by the adaptive (FGK-style) Huffman algorithm of 0x80665ce: starting from
  a tree with only the "not yet transmitted" (NYT, symbol 256) leaf, every byte value v = 0, 1, …, 255 is inserted
  `freq[v]` times, in that order, where `freq` is a fixed table of 256 counts at 0x8142d00 (all counts are positive).
  The patch 1.3 MP executable holds the same 256 counts at VA 0x59cb58 and builds its tree the same way; the
  counts are identical to the public `msg_hData` table in Quake III Arena's GPL source (code/qcommon/msg.c).
  The same procedure feeds two identical tree instances (compressor and decompressor); after the start-up the trees
  are never modified again, so in practice the code is static.
- Adaptive insertion, as implemented: nodes carry a weight and sit in a list ordered by weight; nodes of equal weight
  form a block with a shared "leader" pointer. A new symbol splits the NYT leaf into an internal node (left child:
  the NYT leaf, right child: the new leaf, both weight 1). Incrementing a node first swaps it with the leader of its
  block (unless the leader is its own parent), then raises its weight, then continues with its parent; after the
  parent step, if the parent became the node's list predecessor they are swapped in the list as well. Leader
  pointers are recycled through a free list. These ordering rules decide the final shape and must be reproduced
  exactly; the reference values in `tests/test_net.cpp` pin the result (hash of all 256 codes).
- Resulting code lengths range from 2 bits (byte 0) to 11 bits.

### Bit order and framing

- A symbol's code is the path from the root to its leaf, 0 = left child, 1 = right child (0x8066926), written to the
  output least significant bit first within each byte (0x8066210).
- Compressing a buffer (0x8067f4e) writes the codes of all input bytes back to back and returns
  `(bits + 7) / 8` bytes. There is no length prefix and no end marker.
- Decompressing (0x8067fa2) decodes symbols while the bit position is below `8 * inputLength`. Because the padding
  bits of the last byte are decoded too, the output can end with a few extra bytes; the message parser stops at its
  own end-of-message marker first, so they are harmless. Decoding a leaf for symbol 256 yields byte 0.
- Reading past the end of the input inside one code is not guarded in the original; openiw2 treats missing bits as 0.

The general adaptive compress/decompress pair with a two-byte length header (0x8066cf8, 0x8066a4e) is also present but
has no direct callers in the server; it is not part of this protocol. `TODO(spec)` whether the client uses it.

## 4. Netchan (sequenced packets)

Constants: maximum message length 0x20000 bytes (131072), fragment size 1300 bytes (0x514), packet buffer 1400 bytes.

Per-channel state (set up by 0x806b9d0): socket side (client or server), remote address, qport, outgoing sequence
(starts at 1), incoming sequence (starts at 0), dropped count, fragment reassembly sequence / length / buffer, and the
pending outgoing fragment buffer with its length and send offset.

### Packet layout

```
int32  sequence           bit 31 set = this packet carries a fragment
int16  qport              only on packets sent by a client (socket side 0)
-- if fragment bit set --
int32  fragmentStart      byte offset of this piece in the whole message (32-bit in this game)
int16  fragmentLength     0..1300
--
bytes  payload
```

### Sending (0x806bc6c, 0x806ba4c)

- A message shorter than 1300 bytes goes out in one packet with the current outgoing sequence, after which the
  sequence is incremented.
- A message of 1300 bytes or more is copied to the fragment buffer and sent in pieces of up to 1300 bytes, every piece
  with the same sequence and bit 31 set. The sender sends one piece per call; the caller keeps calling while fragments
  remain. The message is complete when the send offset reaches the length **and** the last piece was shorter than
  1300 bytes; so a message whose length is a multiple of 1300 ends with an empty piece. Only then is the outgoing
  sequence incremented.
- Messages larger than 0x20000 bytes are a fatal error.

### Receiving (0x806be8a)

The client's receive path (MP 0x4481b0) is the same: sequence with bit 31 as the fragment flag, the 16-bit qport read
only on the server side, then a **32-bit** fragment start and a 16-bit fragment length.

- Read the sequence, strip bit 31 into a fragment flag; skip the qport on the server side (the server matches the
  sender by address and qport before calling this, see 0x80960e2); read the fragment header if present.
- Packets whose sequence is not greater than the last accepted one are dropped (out of order / duplicate).
  The gap `sequence - incoming - 1` is stored as the dropped count.
- Fragments: a new sequence restarts reassembly at length 0. A piece whose start differs from the bytes collected so
  far is dropped (a lost piece makes the whole message unrecoverable; the sender does not resend). A negative length,
  a length running past the packet, or a total above 0x20000 is illegal and dropped. A piece of exactly 1300 bytes
  means "more to come" and nothing is delivered yet. A shorter piece completes the message: it is rebuilt as
  `int32 sequence` (without the fragment bit) followed by the reassembled bytes and handed on, with the read
  position placed after that leading sequence. A reassembled message larger than the destination buffer is dropped.
- When a packet (or a completed message) is accepted, the incoming sequence becomes its sequence.

### Game payload framing above the netchan (for E2)

- Client to server (read in 0x80960e2): after the netchan header, `uint8 serverId`, `int32 messageAcknowledge`,
  `int32 reliableAcknowledge`, then the rest of the packet is scrambled (0x8097562) and Huffman-compressed (0x809114e).
  The decompressed stream is a sequence of 3-bit operation codes: 0 = user commands with delta, 1 = user commands
  without delta, 2 = a reliable client command (more operations follow), 3 = end.
- Server to client (0x809aba2, 0x8097626): the first 4 bytes of the message are sent as they are; the rest is Huffman
  compressed, then scrambled (0x80974d8) from byte 4 onward, then handed to the netchan.
- Scrambling (0x80974d8 server to client, 0x8097562 client to server; the client mirrors both): each byte i of the
  region is XORed with a running **8-bit** key. Before byte i the key is XORed with `c << (i & 1)` (low 8 bits kept),
  where `c` is the next character of a per-message string; at the string's terminating zero the string restarts
  (an empty string leaves the key unchanged). There is no character substitution. Scrambling is its own inverse.
  - Server to client: initial key = low byte of `challenge XOR sequence`, where sequence is the netchan outgoing
    sequence of this packet (the first field of the channel state, read before the transmit increments it). The
    string is the text of the last reliable command received from this client (server client structure +0x20844).
    The region starts after the leading 4 bytes.
  - Client to server: initial key = low byte of `challenge XOR serverId XOR messageAcknowledge` (client structure
    +0x20820, +0xae710, +0x20818). The string is the server's own reliable command number `reliableAcknowledge & 127`
    (128 slots of 1032 bytes at +0x40c). The region is everything after the 9 header bytes.
  - Observed: with these keys every gamestate and snapshot of the 1.3 server decodes, also after reliable commands
    were exchanged in both directions.
- The leading 4 bytes of a server message are the number of the last reliable client command the server received
  (observed: it follows the commands sent by the test client).

### Server to client operations (MP 0x4143e0)

The client decompresses the whole remaining message (MP 0x444050) and then reads **whole bytes** as operation codes:
0 = nop, 1 = gamestate, 4 = server command, 5 = download (`TODO(spec)`), 6 = snapshot, 7 = end of message. Any other
value is a fatal "illegible server message". Loose bits inside an operation use the mixed bit packing of section 2 on
the decompressed buffer.

- Server command (4): `int32 sequence`, string (section 2). Reliable: the client keeps the last 128 by
  `sequence & 127`; its `reliableAcknowledge` is the highest sequence received. The text is a console-style command
  (`v name "value"` sets a client cvar, observed).
- Gamestate (1, MP 0x413bd0): `int32 serverCommandSequence`, then byte-coded entries until 7:
  - 2: configstring: `int16 index` (0..2047, else fatal), big string. The texts are stored back to back in a buffer
    of 0x20000 bytes (overflow is fatal).
  - 3: baseline: entity number in 10 bits (0..1023), then the entity frame body of section 7 relative to an all-zero
    entity state.
  - 7: end, followed by `int32 clientNum` and `int32 checksumFeed` (the base of the user-command key, section 6).
- Snapshot (6, MP 0x4136c0): `int32 serverTime`; `uint8 deltaNum` (0 = not delta-coded, otherwise the frame
  `messageNum - deltaNum`, where messageNum is the netchan sequence of this message); `uint8 snapFlags`; the player
  state (section 7.3); the entity list (MP 0x412fb0); the client list (MP 0x413310 / server 0x809847f).
  - The client keeps 32 frames by `messageNum & 31`; a delta from a frame that is missing or overwritten is dropped
    ("Delta frame too old"), as is one whose entities or clients fell out of the 2048-entry parse rings.
  - Entity list: repeated `number` (10 bits) followed by the entity frame body (section 7) coded against the same
    number in the delta frame, or against the baseline when the delta frame does not have it; number 1023 ends the
    list. Numbers ascend; entities of the delta frame that are not named are carried over unchanged.
  - Client list: before each client frame a 1 bit, then the 6-bit client number and the frame body coded against the
    delta frame's client state (or zero); a 0 bit ends the list.

### Client to server operations (0x809114e)

After the 9 header bytes the rest is unscrambled and Huffman-decompressed; then 3-bit operation codes:
- 2 = reliable client command: `int32 sequence`, string (0x8090bac). The client resends every command after the
  last one the server acknowledged.
- 0 / 1 = user commands with / without a delta snapshot (0x8090e34): `uint8 count` (1..32), then the commands
  (section 6). With op 0 the snapshot the client acknowledged (`messageAcknowledge`) is the delta base for the next
  snapshot; with op 1 the server sends a full snapshot.
- 3 = end.
- `serverId` is the low byte of the `sv_serverid` value of the system-info configstring (index 1).
- The packet's `serverId` decides what is read (0x809114e):
  - equal to the server's (or the client is downloading): the operations above are read;
  - only the high nibble equal (a map restart in progress): nothing is read; a primed client enters the world with
    its last command (0x808f628);
  - otherwise (an older map, or no gamestate yet): nothing is read, client commands included (the client sends them
    again under the new id). Only when the client acknowledged a message newer than the last gamestate
    (`gamestateMessageNum < messageAcknowledge`) the gamestate is sent again ("dropped gamestate, resending"); a
    gamestate still in flight is never sent twice.
- Sending the gamestate (0x808f302) first flushes the pending fragments, sets the client primed (state 3), and stores
  the outgoing sequence as `gamestateMessageNum`. A primed client that sends user commands with the current id enters
  the world. Clients not yet active still get a message every frame (0x809adea: the pending server commands, and a
  snapshot for every state but the zombie one), which is how a lost gamestate is noticed.
- Timing and cadence (VAs in the 1.3 Linux dedicated server): see section 4a. Corrected from an earlier reading: only
  **active and zombie** clients get the pending server commands and a snapshot; primed and connected clients get a
  message with no operation but the end mark.
- Implemented: openiw2's server keeps the states free / connected / primed / active; connected and primed clients get
  a message without commands and without a snapshot; the map-restart
  nibble case is not modelled (a fast restart keeps the server id here). A client still on the old id is also treated
  as such when its stale id happens to equal the new one while it has not been sent the gamestate.

## 4a. Timeouts, send cadence, reliable-command limits (VAs in the 1.3 Linux dedicated server)

Registered at 0x8093c15..0x8093f8b (default, minimum, maximum): `sv_fps` 20, 10, 1000; `sv_timeout` 240, 0, 1800
(seconds); `sv_zombietime` 2, 0, 1800 (seconds); `sv_maxRate` 0, 0, 25000; `sv_floodProtect` true;
`sv_kickBanTime` 300 (float seconds).

**Timeouts** (0x809657e, once per server frame). For each slot a `lastPacketTime` in the future is first pulled back
to the current time; test clients are skipped. A zombie (state 1) whose last packet is older than
`sv_zombietime * 1000` ms becomes free. Any other used slot whose last packet is older than `sv_timeout * 1000` ms
counts the check; when the count of consecutive failing checks **exceeds 5** the client is dropped with `EXE_TIMEDOUT`
and freed at once, without passing through the zombie state; a passing check resets the count. So a client disappears
six server frames after the time limit.

A drop by other means leaves the client a zombie: the slot is not reusable for `sv_zombietime`, except by a reconnect of
the same address. A kick restarts the zombie timer from now (0x808c3a6). Zombies keep getting snapshots and commands in
the original; openiw2 does not send any more after the single drop message.

**Rate and snaps** (userinfo at 0x8090863..0x8090963, stored per client). `rate`: absent → 5000; else clamped to
1000..90000. A LAN address (0x80d5638: loopback, 10/8, 127/8, 169.254/16, 172.16/12, 192.168/16 and the addresses of
local interfaces) gets 99999 instead, unless the dvar at 0x84a8780 is 2 (`TODO(spec)`: taken to be `dedicated`, 2 = internet; the registration was not read). `snaps`: absent
→ 50 ms between snapshots; else clamped to 1..30 and the delay is `1000 / snaps` integer ms. A `sv_maxRate` other
than 0 (raised to 1000 if below) lowers the rate used.

**Cadence** after a client message (0x809aba2..0x809adb8): for a loopback or LAN address the next message is due at
once (time - 1), so it follows the next server frame whatever the rate. Otherwise the delay is
`(min(bytes,1500) * 1000 + 48000) / rate` ms (0x809aad0; `bytes` is the compressed size plus 4), raised to the client's
snapshot delay; a client that is not active and has no download pending waits at least 1000 ms. `SV_SendClientMessages`
(0x809bcff) skips free slots and clients whose time has not come.

**Reliable server commands** (0x8094857, sender 0x8094a10): after incrementing the sequence, if exactly 129 commands are
unacknowledged, the server sends the out-of-band text `disconnect`, marks the client to be dropped with
`EXE_SERVERCOMMANDOVERFLOW` (0x808f2b8, taken at the next send, 0x809ac1e) and replaces the command by
`w "EXE_SERVERCOMMANDOVERFLOW"`.

**Reliable client commands** (0x8090bf8): a sequence not newer than the last is ignored; a gap (more than last + 1)
drops the client with `EXE_LOSTRELIABLECOMMANDS` and stops the message. Flood protection (0x8090cf0): an active client
under `sv_floodProtect` whose command arrives before `nextReliableTime` has the command ignored (only logged) unless the
text starts with `team `, `score ` or `mr ` (those never count); commands that do count set `nextReliableTime` to now +
800 ms; the sequence advances in every case.

**Client message parser** (0x8091332): operation bytes other than 0 (move), 1 (move without delta) and 3 (end) print
`WARNING: bad command byte %i for client %i` and stop the message.

**Delta request** (0x8098628): a request older than 28 messages, or whose entities have left the snapshot ring, is ignored
(the frame is written without delta). openiw2 keeps full snapshots per frame so only the age test applies.

**Ping** (0x80963c8). Non-active client: 999. Otherwise the mean of `ack - sent` over those of the 32 frames with an ack
time > 0 (cleared to -1 when sent); no such frame → 999; above 999 → 999. The ack time of a frame is stored by every
user-command packet naming it (0x8091021), so a later packet that repeats the same acknowledgement raises it.

**Source port** (0x8094bf4, 0x80961eb): a packet whose IP and qport match a client but whose source port differs moves
that client to the new port (and replies go there).

## 5. Connectionless (out-of-band) packets

An out-of-band packet starts with the 4 bytes `FF FF FF FF` followed by text. The sender (0x806c8cc) sends the 4 bytes
and the text **without** a terminating zero. Texts longer than 0x1fffc bytes are refused.

The server dispatcher (0x809594e) checks the prefix, skips packets whose text starts with `pb_` (PunkBuster; openiw2
ignores them), reads one line with the line reader (§2), splits it into tokens and dispatches on the first token.

Tokenizer (0x8060346): up to 512 tokens; whitespace is any byte ≤ space; `//` comments end the line and `/* */`
comments are skipped; a token in double quotes may contain spaces and ends at the closing quote (0x8060455: the sequence `\"` inside a quoted token
keeps the quote in the token and drops the backslash; it does not end the token); an unquoted token ends at whitespace or a quote.

Info strings are `\key\value\key\value…` (lookup 0x80b8108, set 0x80b85ce).

| Request (first token) | Handler | Reply |
|---|---|---|
| `getstatus [challenge]` | 0x8094c84 | `statusResponse\n` + server info string + `\n` + one line per connected player `score ping "name"\n` |
| `getinfo [challenge]` | 0x809537c | `infoResponse\n` + info string |
| `getchallenge <?> [guid]` | 0x808d18e | `challengeResponse <n>` |
| `connect "<userinfo>"` | 0x808e2aa | `connectResponse`, or `error\n…` |
| `ipAuthorize …` | 0x808da2a | (authorize server reply; not used by openiw2) |
| `rcon …` | 0x8097188 | `TODO(spec)` (E2+) |
| `v …` | 0x8094b56 | `TODO(spec)` |
| `disconnect` | — | ignored |

### getstatus

The server info string is the set of server-info cvars with `challenge` set to the request's second token, plus
`pswrd` (`1` if `g_password` is set, else `0`) and `mod` (`1` for a mod / non-main data, else `0`). The player lines use
the format `%i %i "%s"\n` (score, ping, name) and are appended while the total stays below 0x20000 bytes.

### getinfo

Keys, in this order, each only under the stated condition:
`challenge` (second token of the request), `protocol` (118), `hostname`, `mapname`, `clients` (connected players,
if non-zero), `sv_maxclients` (public slots available, if positive), `gametype`, `pure` = 1 (if the server is pure),
`minPing`, `maxPing` (if non-zero), `game` (fs_game, if set), `sv_allowAnonymous`, `con_disabled` (if set),
`pswrd` = 1 (if a password is set), `ff` (scr_friendlyfire, if non-zero), `kc` (scr_killcam, if non-zero), `hw` = 1
(always), `mod`, `voice`, `pb`. Values are decimal integers or strings. openiw2 reports `pb` 0.

### getchallenge

- The server keeps a table of 1024 challenges `{address, challenge, time, …}`. An unknown address takes a free
  slot, or the oldest one; its challenge is `rand() ^ (rand() << 16) ^ serverTime`.
- Original: if the request comes from a LAN address while LAN authorisation is off (the default), the server
  answers `challengeResponse %i` at once; otherwise it first asks the authorize server (`getIpAuthorize`) and answers when
  that reply arrives or times out. A third token (GUID / key hash, up to 32 characters) is stored with the challenge when the request has
  exactly three tokens. The 1.3 client always writes the second token as a literal `0` (format at 0x40e3e0 of the patch
1.3 MP executable). `TODO(spec)`: whether the server reads this token (its handler at 0x8095c52 was not read).
- openiw2: always answers immediately (the LAN path). No authorize traffic.

### connect

The single argument is the client's userinfo. The handler checks, in order: `protocol` must equal 118, else the reply
is `error\nEXE_SERVER_IS_DIFFERENT_VER` followed by byte 0x15 and `1.3\n` (the 0x15 separates a localisation key from
its argument); `challenge` must match the stored challenge for the address (`error\nEXE_BAD_CHALLENGE`); ping limits
(`EXE_ERR_HIGH_PING_ONLY`, `EXE_ERR_LOW_PING_ONLY`); reconnect throttling; password; free slot
(`error\nEXE_SERVERISFULL`). The `qport` key is taken from the userinfo. On success the server creates the netchan for
the client and replies `connectResponse`. Details of slot allocation and the game's own rejection are E2 work.

## 6. Delta-coded user commands

A user command is 28 bytes: `int32 serverTime`, `int32 buttons`, `uint8 weapon`, `uint8 offhand`, two padding bytes,
`int32 angles[3]` (only the low 16 bits are sent), `int8 forwardmove`, `int8 rightmove`, two padding bytes.
`TODO(spec)`: names of the two byte fields are inferred.

Writer 0x8068bc0, reader 0x8068f9e. Every delta is relative to the previous command and protected by a 32-bit key:

- **serverTime** (not keyed): if `to - from` is below 256 (unsigned), a 1 bit and one byte with the difference,
  otherwise a 0 bit and the full 32-bit value.
- **Movement** is sent as a 4-bit class: forward ≥ 11 → 1, ≤ -11 → 2; right ≥ 11 → 4, ≤ -11 → 8 (0x8068b20). The
  receiver turns the class back into ±127 / 0 (0x8068b66). Small movement values are therefore lost.
- Keyed fields: a value of N bits is written as `value XOR key` (N bits). A keyed "delta" field writes a 0 bit when
  the value equals the base, otherwise a 1 bit and the keyed value (0x806871c for N bits, 0x806886e for 16 bits
  written as a whole short). The reader masks the key to N bits (mask table 0x817d880 holds `(1 << n) - 1`).
- Layout after serverTime:
  1. Keyed bit "changed". If 0, the command equals the base apart from serverTime.
  2. Keyed bit "major". It is 1 when the upper buttons (bits 1..18), weapon, offhand or the third angle changed.
  3. Minor case: the key is XORed with the new serverTime, then: keyed bit = buttons bit 0, delta16 angle 0,
     delta16 angle 1, keyed delta 4-bit movement class.
  4. Major case: keyed bit = buttons bit 0, delta16 angle 0, delta16 angle 1, keyed delta 4-bit movement class, all
     still with the **old** key; then the key is XORed with serverTime and the rest follows: delta16 angle 2, keyed
     delta 18 bits = buttons >> 1, keyed delta 7 bits weapon, keyed delta 7 bits offhand.
- Key (0x8090e34): `checksumFeed XOR messageAcknowledge XOR hash(serverCommand[reliableAcknowledge & 127])`, where
  checksumFeed comes from the gamestate and the hash (0x80acaa4) runs over at most 32 characters of the server's
  command text: `h = sum(signed char c[i] * (i + 119))`, then `h ^ (h >> 10) ^ (h >> 20)` (arithmetic shifts).
  Observed: forward movement sent with this key moves the player on the 1.3 server.
- The first command of a packet is coded against a base command rebuilt from the player's current state
  (0x80689a6): all zero, then weapon = player-state byte +212, offhand = byte +208; for angles 0 and 1:
  `(trunc(viewangles[i] * 65536 / 360) & 0xffff) - delta_angles[i]`, kept to 16 bits (angle 2 stays 0); buttons:
  0x100 or 0x200 when pm_flags (+12) has 0x800000 and eFlags (+160) has 8 or, failing that, 4; 0x80 when leanf (+76)
  is positive, 0x40 when negative; 0x1000 when fWeaponPosFrac (+220) is non-zero. Each further command of the packet is
  coded against the previous one.

## 7. Delta-coded entity, client and player state

### Field table

Each structure is described by a table of `{name, byte offset, bits}` entries (12 bytes each). Bit codes:

- `0`: float. Equal → 0 bit. Otherwise 1 bit, then: value 0.0 → 0 bit; an integral value in [-4096, 4095] → 1 bit,
  0 bit, then `value + 4096` as 5 bits followed by one byte with the remaining high bits; anything else → 1 bit, 1 bit,
  raw 32 bits as a long (0x80692dc).
- `-99`: float with a smaller shortcut: integral values in [-512, 511] are sent as `value + 512` in 2 bits plus one
  byte.
- `-100`: angle: 0.0 → 0 bit, else 1 bit and a 16-bit angle as a short.
- `N` (other): integer of |N| bits. 0 → 0 bit; else 1 bit, the low `|N| mod 8` bits as loose bits, then the remaining
  bits as whole bytes, least significant first.

All fields begin with the changed/unchanged bit described above.

### Structure frame (0x80696bc)

- `number` (entity or client number) is written with a fixed bit count; client state frames have an extra leading
  1 bit.
- Removed (`to` absent): number, then a 1 bit.
- Otherwise the writer finds the last changed field (count `lc`, 1-based). If none changed and the caller does not
  force output, nothing is written. If none changed but output is forced: number, 0 bit (not removed), 0 bit (no
  changes). If something changed: number, 0 bit, 1 bit, one byte `lc`, then the first `lc` fields in table order.

Tables (VAs): entity state 59 fields at 0x81417a0, number in 10 bits (0x806984c); archived entity (shared entity
data, begins with `absmin`) 68 fields at 0x8141ae0, number in 10 bits (0x806989a); client state 22 fields at
0x8141f60, number in 6 bits, extra leading bit (0x80698e8). A plain list variant without the frame writes one "any
changed" bit and then every field (0x8069602).

The client holds the same tables (entity MP 0x59bda0, archived entity MP 0x59c068, client MP 0x59c398, player
state MP 0x59c4a0, objective MP 0x59c990): same field order, offsets and bit codes as the server's. Structure sizes:
entity state 240 bytes, archived entity 276, client state 92, player state 9896, objective 28, HUD element 128.
`src/net/netfields.cpp` lists them; see "Field tables in openiw2" below.

### Field tables in openiw2

Each table is an interoperability constant: the protocol fixes the order of the fields (it decides which fields the
`lc` count covers), the bit code of each field, and therefore the byte layout of the structure the coder reads.
openiw2 records exactly those three things, with offsets into its own structure layouts of the sizes given above,
which keep the original field order and offsets so that a delta base received from a 1.3 peer can be used as is.
The names in the tables are only labels: a field that has a counterpart in the GPL Quake III Arena tables
(code/qcommon/msg.c, `entityStateFields` / `playerStateFields`) carries the Quake III name; every other field has a
name chosen by openiw2 that describes its meaning as far as it is known. Meanings below that are not backed by an
address are `TODO(spec)`; the wire behaviour does not depend on them.

- Entity state (59 fields, server 0x81417a0, client MP 0x59bda0): the two trajectories (`pos` for position, `apos`
  for angles; type, start time, duration, base, delta, laid out as in Quake III), entity type and flags, the event
  ring (sequence number, four events, four parameters, plus a single event parameter), animation numbers for legs
  and torso, the weapon, owner / ground / other / attacker entity numbers, a type-dependent index, the solid
  (bounding box) code, a loop sound, a constant light, two free times, a second origin and angle set, and openiw2's
  `leanFraction`, `surfaceType`, `scale`, `damageFlags`, `moveAnimClass`, torso height offset and torso / waist pitch
  angles, and the head icon and its team (head-icon fields: `TODO(spec)` exact use).
- Archived entity (68 fields, server 0x8141ae0, client MP 0x59c068): an entity state as above followed by the
  world bounds (`boundsMin`, `boundsMax`), the server-side flags word and a two-word client visibility mask. Field
  order differs from the entity table; offsets of the shared part are the same.
- Client state (22 fields, server 0x8141f60, client MP 0x59c398): team, body model, the player name as eight 32-bit
  words (`nameBytes[0]` … `nameBytes[28]`, the index is the byte offset within the name), and six attached models
  with their tag numbers.
- Player state (105 fields, server 0x8142520, client MP 0x59c4a0): the Quake III movement core (command time,
  pm type / flags / time, origin, velocity, view angles, delta angles, gravity, speed, ground entity, event ring,
  animations and timers, damage event / yaw / pitch / count, client number, weapon and weapon state), and the
  1.3 additions under openiw2 names: view-height current / target / blend values, weapon timing and position
  fraction, owned-weapon and weapon-slot bit words, rechamber bits, aim spread, breath hold, offhand weapon,
  hint type and hint string, view lock entity and mode, view clamp centre and range, bounding box, ladder normal,
  prone yaw / pitch and torso pitch, mantle yaw / timer / transition / flags, jump start time and height, lean,
  grenade fuse, shell-shock type / start / duration, aim-down-sight delay, compass friend bits and a frame delta
  time. Offsets beyond the Quake III core follow the 9896-byte layout. `TODO(spec)`: the exact semantics of the
  bit words and the hint fields.
- Objective (6 fields, server 0x8142a20, client MP 0x59c990): origin, owning entity number, team and icon material.
- HUD element (32 fields, server 0x8142b80): position (`x`, `y`, `z`), element type, colour and the colour faded
  from, screen and origin alignment, font and font size, material, size, value, text and label indices, sort,
  foreground flag, the fade / scale / move animations (start time, length and the values they start from), and a
  time and duration.

### 7.3 Player state (writer 0x806a1c0..0x806aac0, reader MP 0x446ca0)

Fields (table of 105 at 0x8142520): `uint8 lc` (fields up to the last changed one), then for each of the first `lc`
fields a changed bit and, when changed, the value. Unlike the entity coder there is **no zero shortcut** for integers
and floats:
- float (0): 0 bit, then `value + 4096` as 5 loose bits and one byte (integral values in [-4096, 4095]);
  otherwise 1 bit and the raw 32 bits as a long. The reader rebuilds `low5 + (byte - 128) * 32`.
- angle (-100): 0 bit when the raw value is 0, else 1 bit and a 16-bit angle as a short.
- integer: the low `|N| mod 8` bits as loose bits, then whole bytes. A negative bit code marks a signed field: the
  reader sign-extends from bit `|N| - 1`.
Fields after `lc` are copied from the delta base. Then the extra sections, each guarded by one bit:
1. stats (+300, 6 int32): bit, 6-bit change mask; stats 0, 1, 2 and 4 as shorts, stat 3 in 6 bits, stat 5 as a byte.
2. ammo (+324, 64 int32): bit, then four groups of 16: per group a bit, a short mask, one short per masked slot.
3. ammo in clip (+836, 64 int32): the four groups as above **without** a leading section bit: they are always
   present, also when the ammo section's bit is 0 (confirmed by the spec-only decoder of section 9 on the 1.3
   server's stream).
4. objectives (+1508, 16 × 28 bytes): bit, then per objective its state (int32 at +0) in 3 bits and the plain field
   list of section 7 (6 fields, table 0x8142a20) without force.
5. HUD elements: bit, then two arrays of 31 elements of 128 bytes, the one at +5928 first, then the one at +1960
   (0x8069f84): count of leading elements whose type (int32 at +0) is non-zero in 5 bits; per element the index of the
   last changed field in 5 bits and the fields up to and including it with the entity field coder (table of 32 at
   0x8142b80). `TODO(spec)`: what the reader does with elements at or after the count.

## 8. Implementation notes (openiw2)

`src/net` implements §2–§5 as a library without game integration: the message buffer, the Huffman coder (tree
built at start-up from the frequency table), the netchan with fragmentation, connectionless parsing and replies
(getinfo / getstatus / getchallenge / connect checks of protocol and challenge), the usercmd delta coder, the field
encoder of §7 for caller-supplied tables, and a UDP socket for macOS and Linux.

`src/net/scramble.cpp`, `src/net/playerstate.cpp`, `src/net/netfields.cpp` and `src/net/client.cpp` (a test
client: connect, gamestate, reliable commands, user commands, snapshot decoding) implement sections 4–7.

`src/net/server.cpp` is the dedicated-server core: the connectionless handshake of section 5 (a slot is taken on
`connect`, a reconnect from the same address and qport reuses it, `error\nEXE_SERVERISFULL` when none is free), one
netchan per client, the gamestate (resent while the client has not acknowledged it), reliable commands in both
directions, user commands (a connected client that acknowledged the gamestate and sends commands enters the world),
snapshots every 50 ms delta-coded against the frame the client acknowledged (32 frames kept), a `disconnect` client
command and a silence timeout. The game sits behind an interface (configstrings, baselines, connect / begin / command /
think / disconnect, one snapshot view per client). `tools/iw2server` runs the multiplayer World behind it: user
commands drive `playerMove` (src/game/pmove.cpp) and the player entity's origin, the snapshot carries origin, velocity,
view angles and the other players. Section 11 lists what follows the 1.3 layout and what is still open
(rate, snaps and the drop and timeout rules: section 4a).

`src/net/gameclient.cpp` is the game client of section 10 (non-blocking connection states, configstring updates,
server and client commands, a history of 64 user commands, the snapshot pair for interpolation) and
`src/netgame/netmove.cpp` the movement both sides share: `tools/iw2server` runs every user command with it and the
client predicts with it, so the prediction matches the server exactly when the start state does. `iw2view +connect`
shows the map without its gametype (src/game/mpremote.cpp): the local player predicted, the others as third-person
puppets. Section 11 describes what the snapshots carry; section 12 the LAN browser.

`tools/iw2netinfo` answers getinfo / getstatus / getchallenge on a UDP port (`serve [port] [seconds]`) or sends a
query to a server (`query host[:port] getinfo|getstatus`).

## 9. Interoperability check

`tools/iw2netclient host[:port] [seconds] [-v] [-f] [-c command]` connects to a server without authorisation (the
server must accept LAN connections without a key and run with `sv_pure 0`), prints the configstrings, decodes every
snapshot and reports the player's origin; `-f` sends forward movement, `-c` a reliable command. The test
`net_interop` runs it when `IW2_TEST_LNXDED=host:port` names a running 1.3 dedicated server (see
docs/debug-flags.md). Observed with the 1.3 Linux dedicated server on mp_toujane (dm): 282 configstrings, 38 baselines,
continuous delta snapshots with 27 entities, server commands in sequence, a spectator moving with `-f`, and a chat
command echoed by the server log.

**Bots on the 1.3 server.** `addtestclient` is a script function of the 1.3 dedicated server (function table entry
0x81875bc, handler 0x8116bd8), not a console command, so the rcon command does nothing there; a reference run with
bots uses a private copy of the gametype script that calls it and answers the team and weapon menus for the new
client. iw2server's console command `addtestclient` and `--bots` are openiw2 extensions. With bots the differential
test starts iw2server with as many bots as the reference has other clients and checks that each has a client state
and a player entity numbered by its client number.

**Spec-only decoder.** `tests/specdecoder.cpp` reads the server-to-client stream with code written only from
sections 2–7 (its own bit reader, Huffman tree, unscrambling, netchan, gamestate, snapshot and delta readers; it
shares only the byte counts and field tables). `tests/test_net_specdecoder.cpp` (ctest `net_specdecoder`) lets it
read, packet by packet, the stream the test client receives from iw2server (with a bot) and, with
`IW2_TEST_LNXDED`, from the 1.3 server, and requires every configstring, baseline and snapshot to be equal to the
test client's decode.

**Differential test.** `iw2netclient -s --dump file` runs a fixed scenario (after the first snapshot: `mr <serverId> 1
allies`, `mr <serverId> 2 sten_mp`, forward movement, two presses of attack, `score`) and writes every decoded item as
text. `tests/test_net_wirediff.cpp` (ctest `net_wirediff`, with `IW2_TEST_LNXDED`) runs it against the 1.3 server and
against `iw2server` on the same map and gametype and compares: the server- and system-info keys, every configstring
outside the ranges listed as open in section 11 (index and text), the number, type, model, weapon and position of
every baseline openiw2 sends, `snapFlags`, the player state's weapon fields (current, offhand, owned bits) and the
fields the 1.3 server always fills, the kind of the client state's body and attached models, and every `v` command.
Status on mp_toujane (dm, tdm, sd in the first round, ctf, hq): every configstring and every baseline equal, no
field difference in the compared set. With `IW2_TEST_ROUNDS` set and an sd reference whose bots leave each round (players
on both teams), the test instead watches both servers for 80 s and requires a round restart on each (configstring
13 with the restart time and both snapshot flag values); The round-start voice aliases (sayObjective, two seconds after the round
starts) are registered by both servers; a client connecting later finds them in the gamestate.

## 10. Game client (connection, server commands, prediction)

Sources: VAs in the patch 1.3 MP executable (`MP 0x4…`) and the 1.3 Linux dedicated server (`0x80…`).

### Connection states

The client walks through these states; the connect screen shows `EXE_AWAITINGCHALLENGE` and
`EXE_AWAITINGCONNECTION` for the first two (MP 0x53937e):
1. challenging: `getchallenge` is sent until `challengeResponse` arrives.
2. connecting: `connect "<userinfo>"` is sent until `connectResponse` arrives; the netchan is created then.
3. primed: the gamestate (op 1) arrived; the client sends user commands without a delta snapshot and waits for its
   first snapshot.
4. active: a snapshot arrived; from now on the view follows the snapshots.
The client repeats the current handshake packet when 3000 ms have passed since the last one (0x40e5a3 in the patch 1.3 MP
executable). `cl_connectTimeout` and `cl_timeout` both default to 200 s (0x410a82, 0x410a62); openiw2 keeps its shorter
own connect bound because its connect screen blocks. `TODO(spec)`: the numeric values of the states.

### Server commands

A gamestate is preceded by the reliable commands still pending for the client (as in Quake III's
SV_SendClientGameState): the next user commands are keyed with the text of the last acknowledged command (section 6),
so the client must have that text. `TODO(spec)`: confirm the order in the 1.3 server.

The cgame splits the command text into tokens and dispatches on the **first character** of the first token
(MP 0x4d1b80; anything else prints "Unknown client game command"). The server builds the texts with `%c` and the
letter (e.g. the configstring update below), so the letter is the whole command name:
- `d <index> <text>`: configstring `index` changed (the dedicated server sends it at 0x8092961 with format
  `%c %i %s`, letter 0x64). The text is the rest of the command after the index. Strings longer than the chunk size (0x400 bytes,
  0x8092866) are sent as several commands with the same format: `x` for the first piece, `y` for middle pieces, `z` for the last;
  the client appends `x`/`y` pieces and applies the whole string at `z`. The cgame then reacts per index
  (MP 0x4d0c10): 0 server info, 1..: as listed in the gamestate.
- `t <index> [1]`: open the script menu of precache slot `index` (the dedicated server's `openmenu` method
  0x80fbc94 sends letter 0x74). The menu names are configstrings starting at 1246 (observed: `ingame`,
  `team_britishgerman`, `weapon_british`, `weapon_german`, `serverinfo_dm`, … at 1246..1255 on mp_toujane).
- `v <name> <value>`: set a client cvar (MP 0x4d1af0; docs/spec/mp.md "Server to client").
- `e` / `f`: one print routine (MP 0x4bf650); `c` / `g`: another (MP 0x4bf770). `h` chat, `i` team chat (both prefix
  the sender name, MP 0x4d1550). `TODO(spec)`: which of the print letters is the console line, the HUD line
  (iprintln) and the bold centre line (iprintlnbold), and the close-menu letter (candidates `u`, MP 0x4d12e0).
- `b`: the scores (dedicated server 0x80fdf08, client parse MP 0x4d02b0), the answer to the client command `score`;
  format and order in docs/spec/mp-scoreboard.md.

### Client commands

Reliable client commands (op 2) are console-style texts. The script menus send `mr <serverId> <menu index>
<response>` (docs/spec/ui.md); chat is `say <text>` / `say_team <text>`. `TODO(spec)`: the exact chat command names
on the wire.

### Client time

VAs in the patch 1.3 MP executable. The client keeps a server-time estimate for drawing and for its user commands:
- Per client frame (0x403080): `clientTime = realtime + timeDelta`, never below the value of the previous frame.
  When it reaches `newest snapshot serverTime - 5` the client marks the frame as extrapolating.
- First snapshot (0x402ec0): `timeDelta = snapshot.serverTime - realtime`, the floor is the snapshot time.
- Every later snapshot (0x402d70, once per received snapshot; the previous snapshot's serverTime is kept by the
  parser 0x4136c0 before the new one replaces it): the target is
  `snapshot.serverTime - realtime - (snapshot.serverTime - previous.serverTime) - 5`, i.e. one snapshot interval
  plus 5 ms behind the newest snapshot, so that the drawn time stays between the two newest snapshots. With
  `diff = target - timeDelta`: a positive diff (the client behind the target) first has the interval subtracted,
  unless the interval is over 500 ms; a diff below that is handled by the slow rule. Then `|diff| > 500` resets
  (`timeDelta = target`, the clock floor and the frame time become the snapshot time), `|diff| > 100` averages
  (`timeDelta = (timeDelta + target) / 2`), otherwise the slow rule: an extrapolating frame since the last snapshot
  takes 2 ms off the delta (and clears the mark), else the delta moves 1 ms towards the target. The slow rule runs
  only while a time-scale value the client keeps equals 1 (`TODO(spec)`: which dvar).
- The cgame (0x4dfda0) draws at that time between the newest snapshot at or before it and the next one, by the
  fraction of the interval; when the time lies before the current pair it is raised to the current snapshot. Past the
  newest snapshot openiw2 holds the newest state: the 1.3 server sends the players with `trType 3` and a zero
  `trDelta` (section 11), so a trajectory evaluation there stays at the base (`TODO(spec)`: the MP cgame's entity
  placement without a next snapshot; SP 0x4acca0 is in docs/spec/vehicles.md).
- A reset can move the clock back: when the server's time falls behind (a long frame, frames without time) the
  client's clock runs on and the next snapshot resets it back to that snapshot. The commands follow the clock (the
  last command time is lowered to the new floor), so they are older than the last command the server ran.
- Server (0x8091091..0x8091147 in the 1.3 Linux dedicated server, the loop over a packet's commands): a command
  newer than the packet's last one is skipped (sent before a reset), and so is one not newer than the last command
  run; the game then clamps the command's time to the level time - 1000 .. + 200 and the step to 200 ms
  (0x80f5d2e; openiw2 does both in the game's client think). openiw2 adds one exception: after a jump of the server's
  own time (it fell over 500 ms behind its frame schedule, or its time fell over 500 ms behind the real time) each
  client may, for 3 s and once, run a command more than 400 ms older than the last one run, which then becomes the last
  command time. Its step is 0 (the step clamp), the commands after it move normally. The player does not stand still
  until the clock has caught up, and without a jump old commands sent again never run.
- Commands never decrease: a command below the previous one carries the previous one's time (two commands in the
  same client millisecond or while the clock is held at its floor carry the same time; the server runs the first).
  openiw2 raised such a command to the previous + 1 ms before, so while the clock was held (an average step
  towards a lower target) the commands ran ahead of it and the server ran the player in slow motion.
- User commands carry that same clientTime as `serverTime` (never decreasing; equal times are possible), so the server's antilag rewind
  (docs/spec/mp-hud.md "Antilag") puts the other players where this client saw them when it fired.

### Frame clock

The cgame reads one time per drawn frame, cg.time (0xf708dc): it lies between the two snapshots it interpolates (0x4a4c10, 0x4a4070).
openiw2 (src/game/clienttime.h, `clientRenderTimeMs`; `clientTimeMs` in the viewer): world time minus one server frame plus the
interpolation fraction of that frame, in the local game and on a network client alike. It equals the world time at fraction 1, is
never negative and never goes back across a tick (tests/test_clienttime.cpp). World time and fraction are always the pair stored by
the same frame's tick loop: code before the loop reads the previous frame's value.

What moves continuously reads this clock, not the 50 ms world time, which would step at 20 Hz: the landing dip and the step offset
(0x4ae6c7, 0x4addb0; tests/test_mplanding.cpp), the damage kick, the shell shock's view kick, mouse and blend, the fog, screen blur
and screen fade update, shader and water time, the compass springs and pings, HUD fades and HUD element fades and clocks. The clock
runs up to one frame behind world time, so a client-side stamp read on it is taken on it too (a world stamp would give a negative
time: the blur overshoots, the damage kick flips, the landing dip is cancelled). A stamp from the game, in level time (the shell
shock's start, HUD element times, objective rings, compass pings) stays as it is and is read clamped (`game::elapsedMs`), so the
effect waits until the clock reaches that frame (`TODO(spec)`: inferred from cg.time lying between the snapshots; the order of the
snapshot transition against these reads is not documented). Decisions rather than animation stay on world time: `scr_fade`'s test for an instant fade, the shock's movement flag, its
sounds. `TODO(spec)`: the time the `scr_fade` command itself compares in the original (0x4bec8c).

A network client moves the snapshot's times onto this clock with the offset between it and the client time above
(`heldClockOffset`). The offset is held within 16 ms: both clocks run with real time, and a per-frame or per-snapshot difference
would make a stamp saw by up to one frame.

The client does not assume the server's frame length: the time delta rule works on the observed snapshot spacing, snapshots are
interpolated by their own times, and the search for a re-based trajectory spans the two snapshots of the pair. Its own world
ticks locally at 50 ms; only the offset above ties it to the server's times.

### Server cadence

A snapshot goes to every active client after each game frame (the message pass follows the frame; one snapshot per
server time, so their serverTimes are spaced by the frame, 50 ms at sv_fps 20, and a late server frame yields one
snapshot with the caught-up time, never two with the same time). openiw2 sends a message every second to a client
whose host stopped stepping its world (its pending server commands). `TODO(spec)`: the `snaps` / rate limits.

### Prediction and interpolation

The client's prediction (VA 0x4deae0 in the patch 1.3 MP executable, the cgame's predict routine) runs every frame
unless the player state's movement flag 0x400000 (following) is set or the no-predict dvars are on:
- It starts from a copy of the whole player state of the current snapshot (0x9aa dwords) and runs, through the shared
  movement code, every command of the last 128 whose time lies after that state's `commandTime` and not after the
  newest command. Since the start is the snapshot again on every frame, the same commands give the same states.
- At the command whose time equals the previous frame's predicted `commandTime` it compares the new origin with the
  previous frame's. A difference longer than 0.1 units is a correction: the previous correction is scaled by
  `(cg_errorDecay - (now - errorTime)) / cg_errorDecay` (at least 0; zeroed when cg_errorDecay is 0), the difference
  (old minus new) is added and the time noted. The drawn origin adds the correction scaled the same way, so it fades
  out over cg_errorDecay ms. The dvar's default is 100 (float, registered at 0x4be868 in the patch 1.3 MP executable).
- After the run the player-state events are compared with the previous frame's predicted state (0x4e0340): of the
  last four sequence numbers, an event is handled when its number is at or past the previous state's sequence, or
  when it is one of the previous state's last four and its slot holds another event (a changed prediction). So a
  jump event is handled once, not again on each frame that replays the jump command.

The ammunition is part of that predicted state. The copy of the snapshot's whole player state includes the reserve
and loaded-round arrays (in-memory player state +0x118 / +0x318, section 7.3's wire +0x144 / +0x344), and the
replayed commands run the weapon step of the shared movement code, so a shot or a reload of a command the server has
not run yet is already subtracted from (or loaded into) the predicted counters. The HUD reads the counters there:
the ammo / weapon ownerdraws take the current weapon and its loaded rounds from the predicted player state (the
weapon-name ownerdraw SP 0x4a7810 reads the weapon at SP 0xf70998; the low-ammo ownerdraw SP 0x4a7760 reads the
loaded rounds of the weapon's clip slot from the array at SP 0xf70c00, the predicted player state +0x318). So a
snapshot whose state has not seen a predicted shot cannot raise the drawn magazine: the prediction starts from that
snapshot's counters and re-applies the pending commands' weapon step on top, every frame.

openiw2 (src/netgame/netmove.cpp, `Predictor`): the wire player state carries only part of the movement state
(commandTime, pm_type, origin, velocity, bob cycle; see section 7.3). The rest (ground, jump flag and origin,
pm_time, stance, view height, timers, the event ring) comes from this client's own result for the acknowledged
command, which the predictor keeps per command time; the server's fields are put over it and the newer commands are
run. The step of a command is the difference of consecutive command times, clamped to 0..200 ms, 50 ms for the first.
The ammunition lives outside the predicted movement state here (the view world's inventory, written by the weapon
simulation, which runs once per frame and not once per replayed command), so what each command's weapon step changed
is kept per command time instead (`PredictedAmmo`): the counters before and after the frame's weapon update give that
frame's command its change, and a snapshot's counters are written as the server sent them plus the changes of the
commands after the snapshot's commandTime. Changes up to that time are dropped. A snapshot's own correction of the
counters is not a change of a command, so it is never replayed; the record starts over with each gamestate, as the
command history does. The weapon simulation is stepped by the command's own step (`usercmdStep`, the server's step for
that command), not by the frame time, so client and server reach a shot at the same command.

The two weapon simulations can still run a command with different steps: the server clamps a command's time to the
level time + 200 ms, so after a stall of the server several commands run with almost no time and a later one with the
sum, and the shot falls on another command than the client's. In the original the weapon timer is part of the replayed
state, so the next prediction starts from the server's value. openiw2 runs the weapon once per frame and keeps the timer
of each command (`PredictedWeaponTime`): when a snapshot acknowledges a command, its `weaponTime` (ps+52, ms) in the same
`weaponstate` (ps+216) moves the client's timer by the difference, and the kept timers of the newer commands with it.
The server fills both fields from its copy of the weapon. A command run with a clamped time has no matching record and
is skipped; the next exact one corrects it. A snapshot cannot undo a shot the client has already predicted.

### Client clock after a gamestate

A gamestate (connect, a new map, map_restart) starts the server's time again. The client then clears its clock
(delta, floor), its last command time and the command history its packets and prediction use, as the original's
cleared client state does: commands before the level's first snapshot carry time 0,
and the first snapshot sets the clock. openiw2 kept the old level's command time once, so every command of the new
level went out as "previous + 1 ms" and the server ran the player in slow motion (slow walk, slow falls, weapon
timers lagging the client's).

### Map change

When the server loads another map (map rotation, `map`, a restart that is not a fast restart) it sends every connected
client a new gamestate, as on connect: every client goes back to connected, and its next packet (still carrying the old
server id) gets the gamestate by the rule of section 4; a client that is dead, spectating or loading is treated the
same way, since only the server id and the acknowledgement count. (iw2view's listen server notices the new level by a
level counter, not by the World's address, which a new level may reuse.) The client drops the level it has and loads the map named by the new
server info (`mapname`), then goes on with the snapshots of the new map; the team and weapon menus come again from the
gametype's connect. Implemented: iw2view counts the gamestates (`GameClient::gamestates`) and starts the level load for
each one newer than the one its level was built from that carries another server id (a gamestate sent again
  while the client was still loading is not a new map). TODO(spec): the client's state machine during that load (the
exe's client states 5..7) is not modelled; snapshots arriving meanwhile are applied to the new world.

## 11. Network game content

Sources: VAs in the 1.3 Linux dedicated server (`0x8…`); "observed" means seen in the decoded gamestate and snapshots
of that server on mp_toujane dm with the differential test of section 9.

`src/netgame/worldgame.cpp` is the game behind the server core for both `tools/iw2server` and the listen server in
iw2view (the host's own player stays MP client 0; the host's front end runs the world's frames). The gamestate tells
each client its MP client number, so entity and client-state numbers are MP client numbers for players.
`src/netgame/remoteview.cpp` applies the snapshots to the client's remote World; the remote world keeps the local
view in slot 0, so the server's client 0 takes the local client's number there.

### Configstrings of the 1.3 gamestate

The game keeps its name tables with one search-or-add routine (0x811d300; it fails with "G_FindConfigstringIndex:
overflow") that takes the first configstring, the table size and whether to add a missing name. Index 0 means
"none", so name i of a table is configstring base + i. The bases at the call sites:

| table | base | size | call site |
|---|---|---|---|
| models | 334 | 256 | model index routine (G_ModelIndex error text; the lookup reads 334 + i at 0x811d5bd) |
| sounds | 590 | 256 | 0x811d694 |
| effects (loadfx) | 846 | 64 | 0x811d62d |
| (a table of 256 at 910) | 910 | 256 | 0x8115442, `TODO(spec)` |
| shell shocks | 1166 | 16 | 0x811d660, 0x80fd701 |
| localized strings | 1310 | 256 | 0x811d43c |
| materials (shaders) | 1566 | 128 | 0x811d494 |
| (a table of 32 at 110) | 110 | 32 | 0x811d5f8, `TODO(spec)` |

Fixed configstrings set by the server (SV_SetConfigstring, 0x8092780; constant first argument at the call sites):
0 server info and 1 system info (0x8096d3b, 0x8096d6d; keys in the order observed: server info `g_antilag g_gametype
gamename mapname protocol scr_friendlyfire scr_killcam shortversion sv_allowAnonymous sv_floodProtect sv_hostname
sv_maxclients sv_maxPing sv_maxRate sv_minPing sv_privateClients sv_punkbuster sv_pure sv_voice`; system info
`bg_fallDamageMaxHeight bg_fallDamageMinHeight g_synchronousClients sv_cheats sv_disableClientConsole sv_pure
sv_referencedIwdNames sv_referencedIwds sv_serverid sv_voice sv_voiceQuality timescale`), 2 (0x811b5c5), 3 (ambient
sound, 0x8114b0d), 4..6 (0x811b674, 0x80f68e9, 0x80f690e), 7 (the weapon list, 0x8106f75), 8 (0x810710d), 11..22
(0x811b6ff, 0x80fe4d0, 0x811b5ea, 0x811b68c, 0x81008c9.., 0x8109356 and others), and the menus
from 1246 (section 10). Observed contents: 7 is every registered weapon name separated by single spaces, the default
weapon first, then the turret weapons of the map, then the items in precache order; a weapon number on the wire
(player state current / offhand weapon, owned-weapon bits, ammo and clip slots, entity weapon, user command weapon
byte) is the 1-based position in that list. Status icons (precacheStatusIcon) were observed at 23 and 24, the map's
models in the order of the map's entity list (misc_model included) after model 1 `xmodel/defaultweapon`, then the
script's precacheModel calls with each precached item's world and projectile models at the time of the item's
precache.

Further fixed and table configstrings (openiw2 produces all of them):
- 2..4, 11, 13 from the map's worldspawn (SP_worldspawn 0x811b56a): 2 the constant `cod`, 3 `n\<ambienttrack>`,
  4 the `message` key, 11 `northyaw` (default `0`), 13 the server time of the level's start (observed `0` on a new
  map and the time of the round restart in sd; openiw2 sends the time of its last map_restart).
- 3 is replaced by ambientPlay (`n\<alias>\t\<level time + fade ms>`) and ambientStop (`t\<time>`); 12 is the
  fog of setExpFog / setCullFog (`<near> <far> <density> <r> <g> <b> <ms>`, observed `0 1 0.00015 0.9 0.95 1 0`).
- 8: the registered weapon numbers 0..n as a bit mask in 33 hex digits, the low four bits first (observed
  `fffff3…` for 21 weapons; set right after the weapon list, 0x810710d).
- 22: `\winner\0` (observed; set at 0x8109356, `TODO(spec)`: later values).
- 31..: head icons (precacheHeadIcon, base 30) in precache order (observed: `talkingicon` at 31 in dm, after two
  team icons at 33 in tdm and sd).
- 142..237 dvar names and 238..333 their values (empty values have no configstring): the engine's synced dvars
  (bg_*, friction, inertia*, jump_*, mantle_*, player_*, stopspeed, with their 1.3 defaults), the team banner, colour
  and name dvars and the cvars of makeCvarServerInfo (0x8116bfa), sorted by name ignoring case. Colour dvars print
  each component as byte / 255 with four components (".25 .25 .75" becomes `0.25098 0.25098 0.74902 0`).
- Sounds (base 590): a registered weapon's loop / stop fire sounds (observed for the turret: 591..594). Sounds the
  scripts play at run time are registered then on the 1.3 server (observed 595.. in sd: announcer and voice
  aliases); openiw2 registers every alias the scripts play when it is first played.
- Hint strings (base 1278, index 0 used): a registered weapon's use / drop hint strings, then the texts of
  setHintString (observed `PLATFORM_HOLD_TO_PLANT_EXPLOSIVES` at 1280 in sd).
- Material names are stored in lower case (observed `objectivea` for `objectiveA`).
Names the scripts register later in the game reach the clients as `d <index> <text>` commands.

### Entities

- Numbers: 0..63 clients; a map entity has 71 + its position in the map's entity list (worldspawn 0; observed for
  turrets, script models and the first script brush model: 84, 308, 311, 368, 472, 517). 64..71 are not map entities
  (`TODO(spec)`: what holds them). openiw2 gives entities spawned later the lowest number from 72 that no map entity
  of the world (living or deleted), wire entity or event holds, as for the event entities below: the numbers of
  map-list entries the game never spawns (misc_model, light, info_null) are free. Numbering after the whole list left
  no number on mp_carentan (over 1800 entries), so dropped weapons and grenades were not sent there.
  `TODO(spec)`: the server's free-slot delay.
- Players: entity number = client number, eType 1, clientNum (+0x90) the client, pos.trType 3 with pos.trTime the
  snapshot time and pos.trDuration 50, apos.trType 1, solid 0x66010f (the packed box: 15 sideways, 1 down, 70 up),
  groundEntityNum 1022 on the ground, the weapon number, the event ring. openiw2 also carries the velocity in
  pos.trDelta; eFlags as listed below.
- Client state: name, team (+4: 0 none, as in dm, 3 spectator; 1 axis / 2 allies by openiw2's coding,
  `TODO(spec)`), body model index (+8) and the attached models in attach order (+12..: head, helmet).
- Script models: eType 6 with the model index in typeIndex (0 for a script model without a model); brush models:
  typeIndex the inline model number and solid 0xffffff, eType 6 for script brush models and eType 0 with eFlags 1
  for other triggers (observed for trigger_hurt); a brush entity outside the world's bounds is not sent (observed for the sd bomb trigger, which the map places
  outside the playable area; `TODO(spec)`: the linking rule). Turrets: eType 9, the model in typeIndex and the turret weapon number in weapon
  (observed with angles2[0] and apos.trType 3 besides, `TODO(spec)`). Looped effects (playLoopedFx): eType 8, the
  effect index in scale, the repeat delay in ms in angles2[1], origin2 (0, 0, 1). Radius triggers are in the
  baselines only (eType 0, origin and angles). Every entity of the world is in the gamestate's baselines.
- Temporary event entities: eType = 10 + event (observed: bullet impacts 192 with surfaceType, eventParm and the
  shooter in otherEntityNum; the obituary 208 = 10 + 0xc6 with the means-of-death byte in eventParm, victim and
  attacker as in docs/spec/mp-hud.md). openiw2 sends the obituary so. Event entities take the lowest free number from 72
  (observed for the impacts: numbers of map entities the game freed); openiw2 takes the lowest number from 72 that no
  living map entity, wire entity or other event holds (`TODO(spec)`: the server's free-slot delay).
- Script origins are sent like script models (observed: 617 and 618 on mp_toujane are the script origins of the two
  exploder effects, eType 6, typeIndex 0, angles towards the effect's second point); with them the looped effects
  take the same numbers as on the 1.3 server. A model set by setModel is registered at that call. The
  viewModel field (ps+0xe4) carries the model index of the setviewmodel model; the network client takes the hands from
  it.
- Dropped weapons (weapon_<name> items) are eType 3 with the weapon number in typeIndex (observed; openiw2 sends
  them so). Bodies: cloneplayer takes entity 64 + (counter % 8) (0x811eaa4; the body that held the slot is gone, in a
  local game too) and the body is eType 2 with the client number and eFlags `(old & 2) ^ 2 | 0xa0000` (cloneplayer
  0x80fcc76 at 0x80fcd20; observed 0xa0002, later 0x20002: the body's think, deathAnimDuration ms after the clone, clears 0x80000 (0x80fcc48); openiw2 keeps it that long and its client starts the death animation only with it, docs/spec/mp-hud.md "Bodies in the killcam"). The body's legsAnim and
  torsoAnim are the dead client's player-state animations (ps+0x7c and ps+0x84, copied at 0x80fcf1d..0x80fcf38), so
  the client can pose it in the death animation. openiw2 sends the node number of the death animation in the
  multiplayer animation tree in both (`TODO(spec)`: the 1.3 animation numbering and its toggle bit). The client draws
  a body with its client's body model and the attached models of that client state, plays the animation of legsAnim
  once from the body's first snapshot and keeps its last frame; a dropped weapon has the weapon's world model.
- Player eFlags (observed with two test clients): 4 crouched, 8 prone, 0x40 while firing: the movement code clears it every
  command and sets it when the attack button is held, pm_type is not 5, pm_flags has no 0x1000, the weapon state
  (+0xd8) is 0 or 3 and a further check passes (0x80e8bc6..0x80e8c38; `TODO(spec)`: 0x80ed5fc). The bits 0x20 / 0x40
  cleared at 0x807b3ba belong to the collision world's area nodes, not to an entity state. A client that is not
  playing (spectator, dead) has no entity; openiw2 does the same and no longer sends its own 0x20 bit for players.
  The player state carries the same eFlags (+160; 2 for a spectator) and pm_flags 0x800000 while playing, 0x1000000
  as a spectator (observed; `TODO(spec)`: 0x80000, which appears some time after going prone).
- The remote view finds the map entity of a wire number by the same rule, so both sides spawn the map in the same
  order.
- The network client draws only what the snapshots carry: the world model, the static models compiled into the map
  (misc_model) and the entities of the current snapshot. A map entity the server does not send (one the gametype
  script deleted, such as the objects of the other gametypes: the sd bombs and their targets, the ctf flags, the hq
  radios) is neither drawn nor solid on the client; openiw2 spawns the map locally to keep the numbering, so the
  remote view hides every map entity with a model that the newest snapshot lacks and shows it again when it returns.
  A map turret (eType 9) counts as sent; its aim (pitch, yaw relative to its angles) is angles2[0] / [1] (docs/spec/turrets.md "Multiplayer").
- Other players are drawn with the body model (+8) and the attached models (+12..+32) of their client state, for
  network players and bots alike (bots have client states like any client).

### Player state

The 1.3 server always fills: commandTime, origin, view angles, view heights (current 60 standing, target), speed 190
and gravity 800, the bounds (-15 -15 0 / 15 15 70 standing), viewLockEntity 1023, hintStringIndex -1,
groundEntityNum (1022 on the ground), clientNum, hintType (+0x598) and hintStringIndex (+0x59c) from the client's use search (mp.md "Use search of every client"; 0 and -1 when it finds nothing), the weapon fields and health in stats[0] / max health in stats[2].
Weapon fields: current (+0xd4) and offhand (+0xd0) weapon numbers, the owned-weapon bits (+0x544, bit = weapon
number), the slot word (+0x554: the weapon number of slot k in byte k, primary in byte 1, primaryb in byte 2; the
1.3 server's setWeaponSlotWeapon for primaryb with an empty primary gives into primary and then moves the byte to
primaryb, 0x80fc285..0x80fc30d), the first-person model index (+0xe4, viewModel), reserve ammo (+0x144) and loaded
rounds (+0x344) by ammo slot and clip slot: observed, the slot of a weapon is the position of its ammoName (clipName)
among the distinct names of the weapon list in list order, 0-based (webley 4, sten 5 on mp_toujane);
`TODO(spec)`: the assignment routine (near the "BG_FillInAmmoItems" error text). Open difference: the pistol of
setWeaponSlotWeapon has a loaded clip on the 1.3 server and an empty one in openiw2 (`TODO(spec)`: the give's clip
fill).
openiw2 fills the same; boundsMax[2] stays 70 in every stance (observed). HUD elements: the 1.3 server puts
the elements of a default (archived) newHudElem at +0x7a8 (observed: the score and status elements); openiw2 puts
archived elements there and elements with `archived` false at +0x1728 (`TODO(spec)`: confirm with a non-archived
element from the 1.3 server). `snapFlags` is 4 on the
first map; the bit flips with every map_restart (observed with the 1.3 server after sd round restarts) and openiw2
flips it on a new map and on a map_restart.

### openiw2 choices (no 1.3 counterpart observed yet)

- Script entities with a model that are spawned at run time (grenades, flags, the bomb, bodies) are eType 6 with their
  model index (`TODO(spec)`: the 1.3 missile entity type).
- Trajectories: a grenade, a rocket and a falling dropped weapon carry their movement in `pos` (and a turning
  grenade in `apos`) as the Quake III trajectory the server itself runs them with (SP 0x4fae80, docs/spec/grenades.md;
  the 1.3 server and client both hold the trajectory evaluator, `TODO(spec)`: their VAs and the MP cgame's use of
  it): type 5 gravity (`base + delta * t - 400 t^2` in z, t in seconds since `trTime`) for a flying grenade or
  weapon, type 2 linear for a rocket and for a grenade's angles, type 3 linear stop (the move ends after
  `trDuration`) for a sliding weapon, type 0 at rest with the base at the origin. A network client does not step
  such a model from snapshot to snapshot: it evaluates the trajectory of the older snapshot of its pair at its drawn
  time, every rendered frame (`RemoteView::placeTrajectories`), so a thrown grenade flies at the frame rate. A
  bounce or a stop re-bases the trajectory on the server at the hit point with the time of the frame's end (the
  missile waits there for the rest of that frame); when the newer snapshot of the pair carries such a re-based
  trajectory, the client ends the old path where it passes closest to the new base (searched over the frame before
  the new start time) and holds the model there until the new path starts, instead of running the old path through
  the wall and back (openiw2 rule; `TODO(spec)`: the MP cgame's handling). A model the older snapshot lacks (just
  thrown) and any path evaluated before its start time stay at the base. A grenade held in a hand (cooking) is sent
  stationary at its origin, not on its throw parabola.
- Missile explosions: an event entity of type 10 + 0xbc (grenade) or 10 + 0xbd (rocket) at the explosion point
  (pos.trBase), the weapon number in `weapon`, the surface type in `surfaceType`, the direction in origin2, kept 1 s
  and numbered as the other event entities. The client plays each (number, `time`) pair once: the surface sound and
  impact effect of the projectile's explosion type and the weapon's projExplosionEffect / projExplosionSound, as a
  local explosion does, without damage. `TODO(spec)`: the 1.3 server turns the missile entity itself into the event
  and sends the direction as a direction byte in eventParm.
- HUD elements: the elements the client sees (all, its own client elements, its team's), up to 62. Types: 1 text, 2
  value, 3 timer, 4 material, 5 timer up, 8 / 9 clock, 13 waypoint; alignments, fonts and colours (RGBA bytes) as
  small indices of openiw2's tables; times in server time, shifted by the client. Texts are localized-string indices;
  a plain (not localized) text is stored with a leading `"` in the localized-string table.
- Objectives: state, position, team and icon in the objective array.
- Obituaries: an event entity of type 10 + 0xc6 (kept 1 s, numbered as the other event entities above), victim in
  `otherEntityNum`, attacker in `attackerEntity` (1022 none) and the byte of docs/spec/mp-hud.md in `eventParm`; the
  client plays each (number, `time`) pair once.
- Server commands: script menus `t` / `u`, prints `f` / `g`, chat `h` / `i` (docs/spec/mp-chat.md), `v` for
  setClientCvar, all to the receiving client; the client's `say` / `say_team` go to the game's say function. A
  localized message carries its parameters behind 0x15 (observed: `MP_CONNECTED` 0x15 name, `v cg_objectiveText`
  with `MP_GAIN_POINTS_BY_ELIMINATING` 0x15 `50`). Further letters (VAs in the 1.3 Linux dedicated server):
  `K` from closeInGameMenu (0x80fbed2); `I "1"` when giveWeapon gives a weapon the player did not have (0x80f9b8c,
  0x80f9cf5); `C <weapon number>` when the offhand weapon changes (0x8120e44, from the give at 0x8120c0f /
  0x8120c9d and a script method at 0x80fa381); openiw2 sends all three (`C` from the change it sees each frame). The 1.3 server sent the connect message
  with a space instead of a command letter (observed), openiw2 sends `f`.
- A dropped client (kick, clientkick) gets the letter `w` with the reason in quotes (0x808f252, e.g. `EXE_PLAYERKICKED`;
  a second variant at 0x808f226 sends the client's name, `^7`, the reason and ` PB`). The openiw2 client disconnects
  with that reason and can connect again. The client treats it as an error drop (see "Client, server command `w`" below).
- The synced dvars of configstrings 142..333 (section 11) are the client's dvars: the openiw2 client sets each one when
  its value changes (the serverinfo menus read `ui_<gametype>_scorelimit` / `_timelimit`, made server-info cvars by the
  gametype scripts, e.g. `dm.gsc:127-135`). `TODO(spec)`: the 1.3 client routine that applies them.
- The weapons of network players run on the server (section 13).
- Hosting from the menu: the original registers the latched dvar `dedicated` (int 0..2, MP 0x434040); when a latched
  change takes effect (checked every frame, MP 0x434dc0) a non-zero value becomes read-only and the client is shut down
  (CL_Shutdown, MP 0x4116d0), leaving a server without a local player; value 2 also talks to the master server (MP
  0x4b8860). Both a listen server and a dedicated one take LAN clients. openiw2: a game started from the menu always
  takes network clients; with the dedicated choice the window closes and the process becomes the server with a
  console window (docs/spec/mp-ui.md "Dedicated start").

## 12. LAN server browser

The join-server menu (`ui_mp/joinserver.menu`) lists `FEEDER_SERVERS` (feeder 2) in 11 columns; the uiScripts
`RefreshServers`, `JoinServer` and friends are dispatched by name in the MP UI script handler (VAs in the patch 1.3
MP executable: the `JoinServer` name is referenced at 0x53588c). openiw2 (`src/net/lanbrowser.cpp`): a refresh sends
`getinfo xxx` to the broadcast address on ports 28960..28963 and collects every `infoResponse`; the columns are 2
host name, 3 map, 4 players `clients/sv_maxclients`, 5 game type, 10 ping (time to the reply), the icon columns stay
empty. `JoinServer` connects to the selected row and loads its map as a network client. `TODO(spec)`: the 1.3
client's LAN port range, sorting (`ServerSort`), favourites, filters and the icon columns.

## 13. Network players in combat

Sources: VAs in the 1.3 Linux dedicated server (`0x8…`) and in the patch 1.3 MP executable (`MP 0x4…`).

**Buttons.** The user command's buttons are the bits of section 6 (the base command at 0x80689a6 shows prone 0x100,
crouch 0x200, lean 0x40 / 0x80 and aim 0x1000); the rest are the client's button bits of docs/spec/weapons.md:
attack 0x1, melee 0x4, use 0x8, reload 0x10, use-or-reload 0x20, jump 0x400, stance held 0x2000, binoculars 0x4000,
hold breath 0x8000, frag 0x10000, smoke 0x20000. The movement (`inputFromUsercmd`, shared with the client's prediction)
takes the moves, angles, jump, stance, lean, aim and hold-breath bits. Hold breath (MP 0x4f2aa0, the same rules as the
SP 0x4dd350) needs, besides the 0x8000 bit (usercmd +8) and aim fraction 1, a weapon with an aim overlay reticle (def
+0x278 non-null) that is not of class item (def +0x7c != 9); its flag is pm_flags 0x4000, the sway scale ps+0x5b4 and
the timer ps+0x5b8 (wire fields breathHoldScale / breathHoldTimer). Both the server's movement and the client's prediction
therefore take that weapon test from their weapon side (`WeaponMove::breathWeapon`); without it the scope sway is never
steadied in a network game. The cgame breath sounds (MP 0x4d5e30) read pm_flags 0x4000 of the predicted state. Script queries `useButtonPressed`,
`attackButtonPressed` and `meleeButtonPressed` read a network player's command buttons (masks 0x28, 0x1, 0x4 as for
the local player); `isOnGround` reads its movement state.

**Weapon byte.** openiw2 sends the selected weapon as its weapon number (the 1-based position in configstring 7, 0
none; section 11). The
server takes it as the weapon to change to only when it changed since the previous command and the player owns the
weapon; a scripted weapon change (`$weapon`) wins otherwise. `TODO(spec)`: confirm the user command's byte against
the 1.3 client (the player state uses the same numbers) and the offhand byte.

**Server side (openiw2).** Every network player has the weapon state machine of the local player (PM_Weapon,
docs/spec/weapons.md) fed by its commands: fire, aim, reload (press of 0x10 / 0x20), weapon change, melee, frag and
smoke (cooking as for the local player), binoculars. Its shots, melee blows, grenade throws and grenades that go off in
the hand use the same game functions as the local player (FireWeapon with the hip / aim spread, the melee traces, the
throw, the in-hand explosion), so damage, death, obituaries and the script callbacks are the same. A dead player
(pm_type 4) holds nothing; the next life starts with a fresh weapon state. `disableWeapon` puts the weapon away.
Lag compensation: a network client's bullets are fired with the antilag rewind (docs/spec/mp-hud.md "Antilag").

**Entity events.** Event numbers are indices of the event-name table at 0x8183da0 (pointers to the names, in order):
0x92 no ammo, 0x96 reload, 0x97 reload from empty, 0x9a raise, 0x9b put away, 0x9e fire, 0xa0 fire (last shot),
0xa1 rechamber, 0xa2 eject brass, 0xa3 melee swipe, 0xa4 melee fire, 0xa5 prepare offhand, 0xa6 use offhand (the
throw), 0xad / 0xae melee hit / miss, 0xb5..0xba bullet hits, 0xbb grenade bounce, 0xbc grenade explosion, 0xc5
grenade suicide, 0xc6 obituary (as used in section 11). A player's events travel in its entity's event ring
(`eventSequence` +164, `events[4]` +168, `eventParms[4]` +184, as in the Quake III entity state): openiw2 sends fire
(0x9e / 0xa0) for every player that shoots (host, bots, network players) and for network players also reload, melee
swipe and fire, throw and suicide; the parameter is the weapon byte above (`TODO(spec)`: the original's parameters).
The client plays each new event once: the shooter's weapon, the fire sound (positional), the compass ping and the
third-person fire animation, and it traces the shot from the shooter's eye along its view (pitch in `apos.trBase[0]`)
for the impact effect, without damage (the client's world never applies damage). `TODO(spec)`: the original sends
the bullet impacts as their own events (0xb5..0xba).

**Client side.** The client keeps predicting its own weapon (view model, sounds, its own shots' impacts) and the
movement with that weapon's state (aim, speed scale, spread); ammunition and health come from the snapshots.

## 14. Disconnects and the error dialog

VAs in the patch 1.3 MP executable unless noted; VAs in the 1.3 Linux dedicated server are marked "ded".

- Dropping a client (0x454750; ded 0x808f02e): the server queues the reliable server command `w "<reason>"` for that
  client. When PunkBuster is active and the reason is not a localization key it sends `w "<name>^7 <reason>" PB` instead. Reasons seen:
  `EXE_PLAYERKICKED` (kick / clientkick), `EXE_TIMEDOUT` (ded 0x809666b), `EXE_DISCONNECTED` (the client's own
  `disconnect`).
- Server end: quitting the server shuts it down with `EXE_SERVERQUIT` (ded 0x806126a), `killserver` with
  `EXE_SERVERKILLED` (ded 0x80969e6); every connected client gets the `w` command with that reason.
- Client, server command `w` (0x401710, letter 0x77): without an argument it raises a server-disconnect error with
  `EXE_SERVER_DISCONNECTED`; with `PB` as the third token the reason is formatted through
  `EXE_SERVERDISCONNECTREASON`; else the argument is localized and raised as a server-disconnect error.
- Client, connectionless `error` from the server it connects to (0x40ef41): a drop error with the text after the
  first line (`EXE_SERVERISFULL`, `GAME_INVALIDPASSWORD`, the protocol mismatch `EXE_SERVER_IS_DIFFERENT_VER`, ...).
- Error code handling (0x4324c0): the error text is kept, the client disconnects and the UI returns to the main menu;
  0x4320d0 fills the dvars `com_errorMessage` (the text) and `com_errorTitle`: the localized `MENU_NOTICE` for a
  server-disconnect error or when the text is one of the notice keys of the table at 0x5cef94
  (`EXE_SERVER_DISCONNECTED`, `EXE_DISCONNECTED`, `EXE_SERVERISFULL` and four console-only keys), otherwise
  `MENU_ERROR`. With a non-empty `com_errorMessage` the UI opens `error_popmenu` (ui/error.menu) over the main menu
  (0x4350f0 / 0x4324c0 via 0x53e600). The popup shows both dvars; its onClose runs `uiScript clearError`.
- Password: `ui_mp/password.menu` (opened by the join-server "password" button) edits the dvar `password`; the client
  sends it as the userinfo key `password`, which the game compares with `g_password` (docs/spec/dedicated.md
  section 5).

openiw2: the listen server's host leaving the level shuts the listen server down with `EXE_SERVERQUIT`
(`TODO(spec)`: the 1.3 reason for a host disconnect). A connection the client loses on its own (handshake or snapshot
timeout) shows `EXE_DISCONNECTED_FROM_SERVER` (`TODO(spec)`: the original's text and whether it shows a dialog).
`clearError` empties `com_errorMessage` (`TODO(spec)`: its exact effect).

### Client-side use hint and obituary names

- A network client shows the cursor hint of the player state (hintType +0x598, hintStringIndex +0x59c into the hint
  string configstrings); it never searches its own copy of the map for usable entities, because the server's scripts
  may have removed them (the sd bomb triggers in other gametypes).
  A hintType above 4 is a weapon hint: the weapon number (weapon list configstring) + 4; the client shows that
  weapon's icon and, without a hint string, a weapon item's pickup prompt. Hint strings the scripts set after a
  client connected reach it as `d` configstring commands like the other name tables.
- The weapon hint's pickup prompt (when the hint has no string) is chosen each frame the hint is drawn (0x4c5c20 calls the chooser
  0x4c5a80 in the patch 1.3 MP executable), from the hinted weapon's definition and the player state of that frame (the chooser reads the globals 0x1513d18 and 0x1514198; the player state's base 0x1513c44 is inferred from the known field offsets +0xd4 weapon and +0x554 slot bytes that they match): a weapon of
  class item (9) or with an offhand class (+0x84 non-zero) gives `PLATFORM_PICKUPNEWWEAPON`; so does a primary or primaryb weapon
  (+0x80 of 1 or 2) while either primary slot byte of the player state (+0x554, bytes 1 and 2) is empty; otherwise the player
  state's current weapon (+0xd4) decides when it takes the hinted weapon's slot: it is the hinted weapon, no prompt, else
  `PLATFORM_SWAPWEAPONS`; with another slot, the slot byte of the hinted slot being the hinted weapon gives no prompt, else the
  swap. With no prompt the draw takes its icon-only path. The current weapon is the snapshot's, not the view model's: after a swap
  the player state already holds the new weapon while the old one is still being lowered, and the dropped weapon is hinted with
  the swap text at once. A hint held after the hint is gone (cg_hintFadeTime, 100 ms) belongs to the level that set it.
- An obituary event names the victim and the attacker by their client states (name and team), not by the player
  entities in the snapshot: a dead victim's entity may already be gone.
