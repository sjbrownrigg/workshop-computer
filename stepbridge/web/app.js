// StepBridge Web UI — milestone 3: minimal step view/edit + transport +
// connection diagnostics. Connection/heartbeat/diagnostics patterns ported
// directly from EvoSeq's interface.html (WebMIDI quirks already debugged
// there), not re-derived from scratch. SysEx wire format differs from
// EvoSeq by one byte: 0xF0, 0x7D, DEVICE_ID, command, ...payload..., 0xF7 —
// see sysex_protocol.h.

const MIDI_INTERFACE_NAME = 'StepBridge';
const MANUFACTURER_ID = 0x7D;
const DEVICE_ID = 0x53;

// Mirrors sysex_protocol.h. Keep in sync manually.
const WIRE_REST = 127;
const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

// Standard MIDI naming (note 60 = C4, "Middle C"), matching the convention
// most DAWs use - note values here are sent as actual MIDI note numbers
// (before Live Shift is applied at output time), so this is a direct,
// accurate label, not an approximation.
// Mirrors sequencer.cpp's NearestScaleNote exactly (same scale-degree
// tables, same nearest-with-octave-search logic), so client-side snapping
// while drawing (Draw Scale) and the optimistic local update after Snap to
// Scale both agree with what firmware will actually compute - not an
// approximation. Chromatic returns the note unchanged.
const SCALE_PATTERNS = [
  [0, 2, 4, 5, 7, 9, 11],   // Major
  [0, 2, 3, 5, 7, 8, 10],   // NaturalMinor
  [0, 2, 4, 7, 9],          // Pentatonic
  null,                      // Chromatic - no snapping
];

function NearestScaleNote(note, key, scale) {
  const pattern = SCALE_PATTERNS[scale];
  if (!pattern) return note;
  const relative = ((note - key) % 12 + 12) % 12;
  const octaveBase = note - relative;
  let best = note;
  let bestDist = Infinity;
  for (let o = -12; o <= 12; o += 12) {
    for (const degree of pattern) {
      const candidate = octaveBase + o + degree;
      const dist = Math.abs(candidate - note);
      if (dist < bestDist || (dist === bestDist && candidate < best)) {
        bestDist = dist;
        best = candidate;
      }
    }
  }
  return Math.max(0, Math.min(126, best));
}

// Steps one scale degree up/down (not just nearest) - used for plain
// scroll pitch-nudge when a Draw Scale is active (plan 4.2: "or +- one
// scale degree if draw-scale active"). Chromatic falls back to a plain
// semitone step.
function NextScaleDegree(note, key, scale, dir) {
  const pattern = SCALE_PATTERNS[scale];
  if (!pattern) return note + dir;
  const snapped = NearestScaleNote(note, key, scale);
  const relative = ((snapped - key) % 12 + 12) % 12;
  const octaveBase = snapped - relative;
  let idx = pattern.indexOf(relative);
  if (idx === -1) idx = 0;
  idx += dir;
  let octaveShift = 0;
  if (idx < 0) { idx += pattern.length; octaveShift = -12; }
  else if (idx >= pattern.length) { idx -= pattern.length; octaveShift = 12; }
  return Math.max(0, Math.min(126, octaveBase + octaveShift + pattern[idx]));
}

function NoteName(note) {
  if (note === WIRE_REST) return '';
  const octave = Math.floor(note / 12) - 1;
  return `${NOTE_NAMES[note % 12]}${octave}`;
}
const MSG_FIRMWARE_VERSION = 0x01;
const MSG_TRACK_STEPS = 0x02;
const MSG_PLAYHEAD = 0x05;
const MSG_NUM_TRACKS = 0x07;
const MSG_TEMPO = 0x08;
const MSG_CLOCK_SOURCE = 0x09;
const MSG_TRANSPORT_STATE = 0x0A;
const MSG_ARMED = 0x0C;
const MSG_PANEL_STATE = 0x06;
const PANEL_PAGE_NAMES = ['Down (Utility)', 'Middle (Step Edit)', 'Up (Tempo/Bank)'];
const MSG_ACK = 0x40;
const MSG_NACK = 0x41;
// Corrected range (see sysex_protocol.h) — the original 0x60-0x89 scheme
// put 10 IDs at >= 0x80, which is an illegal SysEx data byte value and
// caused MIDIOutput.send() to throw for MSG_SET_TRANSPORT (0x85 = 133).
const MSG_INTERFACE_VERSION = 0x50;
const MSG_REQUEST_TRACK_STEPS = 0x51;
const MSG_REQUEST_NUM_TRACKS = 0x55;
const MSG_REQUEST_TEMPO = 0x56;
const MSG_REQUEST_CLOCK_SOURCE = 0x57;
const MSG_REQUEST_TRACK_LIVE_STATE = 0x58;
const MSG_TRACK_LIVE_STATE = 0x0B;
const MSG_SET_STEP = 0x60;
const MSG_SET_STEP_GATE = 0x61;
const MSG_SET_STEP_TIE = 0x62;
const MSG_SET_STEP_ACCENT = 0x6C;
const MSG_SET_STEP_RATCHET = 0x6D;
const MSG_SET_LENGTH = 0x63;
const MSG_SET_TIMESIG = 0x64;
const MSG_SET_TIMESIG_IRREGULAR = 0x70;
const MSG_SNAP_TO_SCALE = 0x6E;
const MSG_SET_TRACK_SHIFT = 0x76;
const MSG_TRANSPOSE_NOTES = 0x79;
const MSG_PREVIEW_NOTE = 0x6F;
const MSG_SET_TRACK_MUTE = 0x77;
const MSG_SET_TRACK_SOLO = 0x78;
const MSG_SET_DRAW_SCALE = 0x7B;
const MSG_RANDOMIZE = 0x71;
const MSG_UNDO_RANDOMIZE = 0x72;
const MSG_SLOT_BITMAP = 0x04;
const MSG_REQUEST_SLOT_BITMAP = 0x53;
const MSG_SAVE_SLOT = 0x69;
const MSG_LOAD_SLOT = 0x6A;
const NUM_SAVE_SLOTS = 8;
const MSG_DIAG_EVENT_BATCH = 0x10;
const MSG_REQUEST_DIAG_EVENTS = 0x54;
const MSG_SET_MIDI_CHANNEL = 0x65;
const MSG_SET_MIDI_ENABLED = 0x66;
const MSG_ADD_TRACK = 0x67;
const MSG_REMOVE_TRACK = 0x68;
const MAX_TRACKS = 8;
const NUM_CV_TRACKS = 2; // tracks 0/1 are structural CV+Pulse, never removable
const MSG_GLOBAL_MUTE = 0x0D;
const MSG_REQUEST_GLOBAL_MUTE = 0x59;
const MSG_SET_GLOBAL_MUTE = 0x7A;
const MSG_SET_CV_ROUTING = 0x7C;
const MSG_SET_CV_ROUTING_CAL = 0x7D;
const MSG_CV_ROUTING_STATE = 0x0E;
const MSG_REQUEST_CV_ROUTING = 0x5B;

const SCALE_NAMES = ['Major', 'Natural Minor', 'Pentatonic', 'Chromatic'];
const KEY_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const MSG_SET_CLOCK_SOURCE = 0x74;
const MSG_SET_TRANSPORT = 0x75;

const TRANSPORT_NAMES = ['Stopped', 'Paused', 'Playing'];
const CLOCK_SOURCE_NAMES = ['Internal', 'External Pulse', 'MIDI Clock'];

let midi = null;
let midiInput, midiOutput;
let midiActive = false;
let reacquiringMidi = false;
let lastMidiMessageTime = 0;

let numTracks = 2;
let knownTrackDataCount = 0; // how many tracks (0 up to, not including, this) we've already fetched step/live-state data for
const tracks = []; // {length, timeSigNum, steps:[{note,gateLenPct,tied,accent,ratchetCount},...], currentStep}
const attachedGestureTracks = new Set();
const editLockedTracks = new Set(); // client-side only, see plan 4.2 - never sent to firmware
let globalEditLock = false;
let globalMute = false; // master silence-all, overrides per-track mute/solo (see firmware Pattern::globalMute)
let slotBitmap = 0; // bit i = slot i has a saved pattern
let activeSlot = -1; // -1 = none yet this session - see MSG_SLOT_BITMAP

// Live panel state (push-only, no pull-query - see sysex_protocol.h's
// MSG_PANEL_STATE comment). panelScrubStep only means anything while
// panelPage===1 (Middle/Step Edit).
let panelPage = 0;
let panelSelectedTrack = 0;
let panelScrubStep = 0;

// Draw Scale (plan 4.4): client-side only input-assist, never sent to
// firmware. { key: 0-11, scale: 0-3 } per track, or null for free
// chromatic drawing. Independent of (and not constrained by) whatever
// scale a later Snap to Scale targets.
const drawScale = {};

function IsEditLocked(trackIndex) {
  return globalEditLock || editLockedTracks.has(trackIndex);
}

// Client-side only (plan 4.2) - gates which mouse events the gesture
// handlers above act on, nothing more. Never sent to firmware, never
// persisted in a save slot. Lets you perform/watch a track without risk of
// an accidental drag changing it, while every playback animation keeps
// running exactly as before - there's deliberately no separate Play/Edit
// mode, just this opt-in safety net.
function ToggleTrackLock(trackIndex, btn) {
  if (editLockedTracks.has(trackIndex)) {
    editLockedTracks.delete(trackIndex);
    btn.classList.remove('active');
  } else {
    editLockedTracks.add(trackIndex);
    btn.classList.add('active');
  }
}

function ToggleGlobalLock() {
  globalEditLock = !globalEditLock;
  document.getElementById('globalLockBtn').classList.toggle('active', globalEditLock);
}

// ---------------------------------------------------------------------------
// Pull-model request tracking (plan 3.3): a request with no timely reply is
// a visible failure, not silence. One in-flight slot per query type is
// enough at this protocol's size.
const PENDING_TIMEOUT_MS = 750;
const pendingRequests = {}; // msgId -> sentAt

function NoteRequestSent(msgId) {
  pendingRequests[msgId] = Date.now();
}
function NoteResponseReceived(msgId) {
  delete pendingRequests[msgId];
}
function CheckPendingTimeouts() {
  const now = Date.now();
  for (const [msgId, sentAt] of Object.entries(pendingRequests)) {
    if (now - sentAt > PENDING_TIMEOUT_MS) {
      DebugLog(`Request 0x${Number(msgId).toString(16)} timed out (no reply after ${PENDING_TIMEOUT_MS}ms)`);
      delete pendingRequests[msgId];
    }
  }
}

// ---------------------------------------------------------------------------
// Debug log

function DebugLog(msg) {
  console.log(`[StepBridge] ${msg}`); // mirrors the UI panel - useful for copying/searching/persisting past the 200-line UI cap
  const el = document.getElementById('debugLog');
  if (!el) return;
  const line = document.createElement('div');
  line.textContent = `[${new Date().toLocaleTimeString()}] ${msg}`;
  el.appendChild(line);
  while (el.children.length > 200) el.removeChild(el.firstChild);
  el.scrollTop = el.scrollHeight;
}

// ---------------------------------------------------------------------------
// Connection diagnostics (ported from EvoSeq: rAF heartbeat distinguishes a
// paint-pipeline freeze from a JS-execution freeze; raw-vs-valid SysEx
// counts show traffic arriving but failing to parse, vs nothing arriving at
// all — these are genuinely different failure modes that look identical
// from the outside).

let rafFrameCount = 0;
let lastRafTime = 0;
function RafHeartbeat() {
  rafFrameCount++;
  lastRafTime = Date.now();
  requestAnimationFrame(RafHeartbeat);
}
requestAnimationFrame(RafHeartbeat);

let rawMidiEventCount = 0;
let validSysExCount = 0;
let tickerTickCount = 0;

setInterval(() => {
  tickerTickCount++;
  CheckPendingTimeouts();
  const el = document.getElementById('connDiag');
  if (!el) return;
  const now = Date.now();
  const sinceMsg = lastMidiMessageTime ? (now - lastMidiMessageTime) + 'ms ago' : 'never';
  const sinceRaf = lastRafTime ? (now - lastRafTime) + 'ms ago' : 'never';
  el.innerHTML =
    `<b>connected</b> ${midiActive} &nbsp; <b>ticks</b> ${tickerTickCount}<br>` +
    `<b>rAF frames</b> ${rafFrameCount} &nbsp; <b>last rAF</b> ${sinceRaf}<br>` +
    `<b>last message</b> ${sinceMsg}<br>` +
    `<b>raw MIDI events</b> ${rawMidiEventCount} &nbsp; <b>valid SysEx</b> ${validSysExCount}<br>` +
    `<b>pending requests</b> ${Object.keys(pendingRequests).length}`;
}, 500);

// ---------------------------------------------------------------------------
// WebMIDI connection

async function AcquireMidiAccess() {
  try {
    if (midiInput) { try { await midiInput.close(); } catch (e) { DebugLog(`midiInput.close() failed: ${e}`); } }
    if (midiOutput) { try { await midiOutput.close(); } catch (e) { DebugLog(`midiOutput.close() failed: ${e}`); } }
    // A reconnect can drop a frame mid-stream - discard any partial state
    // rather than risk later bytes being misinterpreted as a continuation
    // of a frame from before the reconnect.
    sysexActive = false;
    sysexBuf = [];
    DebugLog('Requesting MIDI access...');
    midi = await navigator.requestMIDIAccess({ sysex: true });
    DebugLog(`Got MIDIAccess - inputs=${midi.inputs.size} outputs=${midi.outputs.size}`);
    BindDevices();
    midi.addEventListener('statechange', () => BindDevices());
  } catch (error) {
    DebugLog(`requestMIDIAccess failed: ${error}`);
    SetConnectedStatus(`WebMIDI error (${error})`);
  }
}

function BindDevices() {
  const currentInput = [...midi.inputs.values()].find(i => i.name.includes(MIDI_INTERFACE_NAME) && i.state === 'connected');
  const currentOutput = [...midi.outputs.values()].find(o => o.name.includes(MIDI_INTERFACE_NAME) && o.state === 'connected');

  if (midiActive && currentInput === midiInput && currentOutput === midiOutput) return;

  if (!currentInput || !currentOutput) {
    if (midiActive) {
      midiActive = false;
      midiInput = undefined;
      midiOutput = undefined;
      SetConnectedStatus('Disconnected');
    }
    return;
  }

  midiInput = currentInput;
  midiOutput = currentOutput;
  midiInput.onmidimessage = HandleMIDI;
  lastMidiMessageTime = Date.now();
  midiActive = true;
  SetConnectedStatus('Connected');
  OnConnected();
}

function SetConnectedStatus(text) {
  const el = document.getElementById('connectedStatus');
  if (el) el.textContent = text;
}

async function ReacquireMidiAccess() {
  if (reacquiringMidi) return;
  reacquiringMidi = true;
  try { await AcquireMidiAccess(); } finally { reacquiringMidi = false; }
}

// Streaming SysEx parser state - persists between HandleMIDI calls. A
// single onmidimessage callback is NOT guaranteed to contain exactly one
// logical message: the OS/browser MIDI stack can coalesce several rapid
// sends into one callback (event.data = [0xF0,...,0xF7,0xF0,...,0xF7]).
// The original implementation assumed exactly one frame per callback
// (checked only data[0]/data[1]/data[2] and the VERY LAST byte for 0xF7),
// which silently spliced one message's 0xF7 terminator and the next
// message's 0xF0/header bytes into the first message's payload whenever
// two arrived together - exactly what surfaced as escalating garbage
// values in the diagnostics event log once that feature added enough
// near-simultaneous traffic to make coalescing likely. Parsing byte-by-byte
// here (mirroring firmware's own ParseMIDIBytes state machine in
// usb_link.h) handles any number of frames per callback, and also a frame
// split *across* callbacks, correctly.
let sysexActive = false;
let sysexBuf = [];

// Largest legitimate single frame the firmware ever sends is 214 bytes
// (SendSysExRaw's kMaxFrame in usb_link.h) - this is deliberately larger,
// as a guard against a DIFFERENT failure mode than the multi-frame-per-
// callback one above: MidiStreamWriteBlocking (usb_link.h) deliberately
// abandons a send if the USB TX FIFO stays full for 200ms, favoring "never
// hang core0" over "never drop a message" (the same lesson as EvoSeq's
// original freeze fix). Under heavy traffic that abandons a send mid-
// message - no closing 0xF7 ever arrives - and without this cap, every
// byte of every subsequent message (starting with its own 0xF0) would get
// appended onto the still-open buffer forever, permanently wedging this
// parser rather than just losing the one truncated message and recovering.
const MAX_SYSEX_BUF = 256;

function HandleMIDI(event) {
  lastMidiMessageTime = Date.now();
  rawMidiEventCount++;
  const data = event.data;
  for (let i = 0; i < data.length; i++) {
    const b = data[i];
    if (!sysexActive) {
      if (b === 0xF0) { sysexActive = true; sysexBuf = [b]; }
      // else: other realtime/channel-voice traffic sharing the bus is
      // expected and not logged, per EvoSeq's finding that logging it just
      // buries genuine anomalies in noise.
      continue;
    }
    sysexBuf.push(b);
    if (sysexBuf.length > MAX_SYSEX_BUF) {
      DebugLog(`SysEx frame exceeded ${MAX_SYSEX_BUF} bytes without a terminator - likely a truncated send under heavy traffic, discarding and resyncing`);
      sysexActive = false;
      sysexBuf = [];
      continue;
    }
    if (b === 0xF7) {
      const looksLikeOurSysEx = sysexBuf.length > 4 && sysexBuf[1] === MANUFACTURER_ID && sysexBuf[2] === DEVICE_ID;
      if (looksLikeOurSysEx) {
        validSysExCount++;
        try {
          ProcessIncomingSysEx(sysexBuf.slice(3, sysexBuf.length - 1));
        } catch (e) {
          DebugLog(`ProcessIncomingSysEx threw: ${(e && e.stack) || e}`);
        }
      }
      sysexActive = false;
      sysexBuf = [];
    }
  }
}

function SendSysEx(command, payloadBytes = []) {
  if (!midiActive) { DebugLog(`SendSysEx(0x${command.toString(16)}) skipped - not connected`); return; }
  const bytes = [0xF0, MANUFACTURER_ID, DEVICE_ID, command, ...payloadBytes, 0xF7];
  midiOutput.send(bytes);
}

function ProcessIncomingSysEx(data) {
  const command = data[0];
  const payload = data.slice(1);
  switch (command) {
    case MSG_FIRMWARE_VERSION:
      NoteResponseReceived(MSG_INTERFACE_VERSION);
      DebugLog(`Firmware version ${payload[0]}.${payload[1]}.${payload[2]}`);
      break;
    case MSG_NUM_TRACKS:
      NoteResponseReceived(MSG_REQUEST_NUM_TRACKS);
      numTracks = payload[0];
      SyncTrackBlocks(numTracks);
      // Only fetch data for tracks we don't already have - re-fetching
      // every track 0..numTracks on EVERY MSG_NUM_TRACKS (e.g. once per
      // Add Track click) compounded the connect-time burst problem badly:
      // clicking Add Track repeatedly while going from 2->8 tracks fired
      // overlapping 6/8/10/12/14/16-request bursts, not just one.
      StaggerPerTrackDataRequests(knownTrackDataCount, numTracks);
      knownTrackDataCount = numTracks;
      for (let t = 0; t < numTracks; t++)
      {
        // MSG_NUM_TRACKS can arrive again (Reconnect/Refresh) - gesture
        // listeners must only ever be attached once per track, or each
        // reconnect would stack another listener and multiply every
        // subsequent send.
        if (!attachedGestureTracks.has(t)) {
          attachedGestureTracks.add(t);
          AttachRibbonGestures(document.getElementById(`track-${t}`), t);
          AttachGateRibbonGestures(document.getElementById(`gate-${t}`), t);
        }
      }
      break;
    case MSG_TRACK_STEPS: {
      NoteResponseReceived(MSG_REQUEST_TRACK_STEPS);
      const track = payload[0];
      const length = payload[1];
      const timeSigMode = payload[2]; // 0=Regular, 1=Irregular
      const timeSigNum = payload[3];
      const irregularGroupCount = payload[4];
      const irregularGroups = payload.slice(5, 5 + irregularGroupCount);
      let offset = 5 + irregularGroupCount;
      // 3 bytes/step: note, gateLenPct, flags (bit0=tied, bit1=accent, bits2-4=ratchetCount-1)
      const steps = [];
      for (let i = 0; i < length; i++) {
        const note = payload[offset++];
        const gateLenPct = payload[offset++];
        const flags = payload[offset++];
        steps.push({
          note,
          gateLenPct,
          tied: !!(flags & 0x01),
          accent: !!(flags & 0x02),
          ratchetCount: ((flags >> 2) & 0x07) + 1,
        });
      }
      tracks[track] = Object.assign(tracks[track] || {}, {
        length, timeSigMode, timeSigNum, irregularGroupCount, irregularGroups, steps,
        currentStep: tracks[track] ? tracks[track].currentStep : 0,
      });
      RenderTrack(track);
      RenderTimeSigControls(track);
      RenderLengthControl(track);
      break;
    }
    case MSG_PLAYHEAD:
      for (let t = 0; t < numTracks; t++) {
        if (tracks[t]) tracks[t].currentStep = payload[t];
      }
      RenderPlayheads();
      break;
    case MSG_TRANSPORT_STATE:
      lastTransportState = payload[0];
      RenderTransport(lastTransportState);
      break;
    case MSG_CLOCK_SOURCE:
      NoteResponseReceived(MSG_REQUEST_CLOCK_SOURCE);
      RenderClockSource(payload[0]);
      break;
    case MSG_ARMED:
      isArmed = !!payload[0];
      RenderTransport(lastTransportState);
      break;
    case MSG_PANEL_STATE: {
      panelPage = payload[0];
      panelSelectedTrack = payload[1];
      panelScrubStep = payload[2];
      RenderPanelState();
      break;
    }
    case MSG_TRACK_LIVE_STATE: {
      NoteResponseReceived(MSG_REQUEST_TRACK_LIVE_STATE);
      const t = payload[0];
      const track = tracks[t];
      if (track) {
        track.shift = payload[1] - 64;
        track.muted = !!payload[2];
        track.solo = !!payload[3];
      }
      RenderLiveState(t, payload[1] - 64, !!payload[2], !!payload[3], payload[6], !!payload[7]);
      RenderAllAudibility(); // solo affects every other track's dimming, not just this one
      // key/scale restore the Draw Scale selector on reload - without
      // this, firmware would still snap panel edits to the previously-set
      // scale, but the Web UI's selector would silently show Chromatic,
      // disagreeing with what's actually happening on the panel.
      RenderDrawScale(t, payload[4], payload[5]);
      break;
    }
    case MSG_GLOBAL_MUTE:
      NoteResponseReceived(MSG_REQUEST_GLOBAL_MUTE);
      globalMute = !!payload[0];
      document.getElementById('globalMuteBtn')?.classList.toggle('active', globalMute);
      RenderAllAudibility();
      break;
    case MSG_CV_ROUTING_STATE:
      NoteResponseReceived(MSG_REQUEST_CV_ROUTING);
      cvTrackRoute = payload[0];
      cvStepRoute  = payload[1];
      RenderCVRouting(
        payload[2] | (payload[3] << 7),  // trackCalMin
        payload[4] | (payload[5] << 7),  // trackCalMax
        payload[6] | (payload[7] << 7),  // stepCalMin
        payload[8] | (payload[9] << 7)   // stepCalMax
      );
      break;
    case MSG_SLOT_BITMAP:
      NoteResponseReceived(MSG_REQUEST_SLOT_BITMAP);
      slotBitmap = payload[0] | (payload[1] << 7);
      activeSlot = payload[2] < NUM_SAVE_SLOTS ? payload[2] : -1;
      RenderSlots();
      break;
    case MSG_DIAG_EVENT_BATCH:
      OnDiagEventBatch(payload);
      break;
    case MSG_TEMPO: {
      NoteResponseReceived(MSG_REQUEST_TEMPO);
      const bpm = (payload[0] << 7) | payload[1];
      // Ground-truth logging while tracking down the BPM/panel-focus
      // glitch correlated with MIDI-enabled track count - shows the exact
      // raw bytes so a wrong decode can be distinguished from a genuinely
      // wrong value computed firmware-side.
      console.log(`[StepBridge tempo] raw=[${payload[0]},${payload[1]}] bpm=${bpm}`);
      RenderTempo(bpm);
      break;
    }
    case MSG_ACK:
      break; // step edits are optimistic-rendered already; nothing more to do
    case MSG_NACK:
      DebugLog(`NACK for command 0x${payload[0].toString(16)}, reason ${payload[1]}`);
      break;
    default:
      DebugLog(`Unrecognized command 0x${command.toString(16)}`);
  }
}

function OnConnected() {
  SendSysEx(MSG_INTERFACE_VERSION, [0, 1, 0]);
  NoteRequestSent(MSG_INTERFACE_VERSION);
  RequestNumTracks();
  // MSG_TEMPO/MSG_CLOCK_SOURCE are send-on-change only on the firmware
  // side - a freshly (re)loaded page has no way to learn the current
  // value until something actually changes it again, confirmed as the
  // cause of the BPM display not appearing after a page reload. Pull
  // both explicitly on every connect, not just track data.
  SendSysEx(MSG_REQUEST_TEMPO);
  NoteRequestSent(MSG_REQUEST_TEMPO);
  SendSysEx(MSG_REQUEST_CLOCK_SOURCE);
  NoteRequestSent(MSG_REQUEST_CLOCK_SOURCE);
  SendSysEx(MSG_REQUEST_GLOBAL_MUTE);
  NoteRequestSent(MSG_REQUEST_GLOBAL_MUTE);
  SendSysEx(MSG_REQUEST_SLOT_BITMAP);
  NoteRequestSent(MSG_REQUEST_SLOT_BITMAP);
  SendSysEx(MSG_REQUEST_CV_ROUTING);
  NoteRequestSent(MSG_REQUEST_CV_ROUTING);
}

function RequestNumTracks() {
  SendSysEx(MSG_REQUEST_NUM_TRACKS);
  NoteRequestSent(MSG_REQUEST_NUM_TRACKS);
}

// Milestone 3 is pull-model only: the firmware never pushes step data
// unprompted (only MSG_PLAYHEAD is send-on-change). If the panel changes a
// step, the Web UI has no way to find out until something re-requests -
// this button is that "something", until a later milestone adds live push
// for step edits too (plan section 4.8 area).
function RefreshAll() {
  DebugLog('Refresh: re-requesting all track data');
  // Forces MSG_NUM_TRACKS's handler to treat every current track as "not
  // yet fetched" - otherwise a Refresh with no actual track-count change
  // would request nothing at all, since knownTrackDataCount would already
  // equal numTracks (see StaggerPerTrackDataRequests's comment).
  knownTrackDataCount = 0;
  RequestNumTracks();
}

function RequestTrackSteps(track) {
  SendSysEx(MSG_REQUEST_TRACK_STEPS, [track]);
  NoteRequestSent(MSG_REQUEST_TRACK_STEPS);
}

// Found via a user report (milestone 13 testing, 8 tracks): firing every
// track's RequestTrackSteps + RequestTrackLiveState back-to-back in one
// synchronous tick - fine at 2 tracks (4 requests), but at 8 tracks
// (16 near-simultaneous requests, each needing a substantial response)
// overwhelmed firmware's USB-MIDI send path badly enough to hang the
// whole comms link until power-cycled. The audio core kept running fine
// throughout, confirming this is a core0/comms-side overload, not a
// core1/audio bug - so the fix belongs here, pacing the requests, not in
// firmware. Spacing them out gives firmware room to fully answer each one
// before the next lands. Only covers indices from `from` up to (not
// including) `to` - the caller (MSG_NUM_TRACKS)
// tracks which tracks' data has already been fetched, so growing from say
// 6 to 7 tracks only fetches the one new track, not all 7 again.
function StaggerPerTrackDataRequests(from, to) {
  for (let t = from; t < to; t++) {
    setTimeout(() => {
      RequestTrackSteps(t);
      RequestTrackLiveState(t);
    }, (t - from) * 40);
  }
}

function RequestTrackLiveState(track) {
  SendSysEx(MSG_REQUEST_TRACK_LIVE_STATE, [track]);
  NoteRequestSent(MSG_REQUEST_TRACK_LIVE_STATE);
}

// ---------------------------------------------------------------------------
// Step view/edit (plan 4.2): sweep-to-draw across the main pitch ribbon and
// a secondary gate-length ribbon beneath it, with modifier keys selecting
// what a click/sweep does. Determined once at mousedown (held for the
// whole gesture, not re-evaluated per cell) so a sweep paints a consistent
// action across every cell it touches:
//   plain        -> set pitch (vertical position, snapped to the rest band)
//   Shift        -> paint rest on/off (target value fixed from the first
//                    cell touched, not re-toggled per cell - a sweep should
//                    set every touched cell to the SAME state)
//   Ctrl/Cmd     -> paint tie on/off (same fixed-target painting)
//   Alt/Option   -> paint accent on/off (disabled on rest steps)
// Wheel: plain = pitch nudge +-1 semitone; Shift = ratchet count +-1 (1-8).
// Gate-length ribbon: plain drag/wheel sets/nudges gateLenPct directly.

const REST_BAND = 0.18; // keep in sync with the panel's own REST band fraction (main.cpp)

function StepIndexAt(container, trackIndex, clientX) {
  const track = tracks[trackIndex];
  if (!track) return -1;
  const rect = container.getBoundingClientRect();
  const rel = (clientX - rect.left) / rect.width;
  let idx = Math.floor(rel * track.length);
  if (idx < 0) idx = 0;
  if (idx >= track.length) idx = track.length - 1;
  return idx;
}

// Shared by the Web UI's ribbon renderer (and, if a future milestone needs
// downbeat-aware logic such as randomize's accent placement, by an
// equivalent firmware-side function then - plan 2.3). Regular mode groups
// steps into fixed-size chunks of timeSigNum; Irregular cycles repeatedly
// through irregularGroups. Either way the trailing group is simply
// shorter if it runs past `length` - no special-casing needed.
function ComputeBarGroups(length, timeSigMode, timeSigNum, irregularGroups, irregularGroupCount) {
  const groups = [];
  let remaining = length;
  if (timeSigMode === 1 && irregularGroupCount > 0) {
    let gi = 0;
    while (remaining > 0) {
      const size = Math.min(irregularGroups[gi % irregularGroupCount], remaining);
      groups.push(size);
      remaining -= size;
      gi++;
    }
  } else {
    const size = timeSigNum || 4;
    while (remaining > 0) {
      const g = Math.min(size, remaining);
      groups.push(g);
      remaining -= g;
    }
  }
  return groups;
}

// Returns a Set of step indices that are the LAST step of a bar group
// (i.e. need a divider after them), excluding the final step of the track.
function BarEndIndices(track) {
  const groups = ComputeBarGroups(track.length, track.timeSigMode, track.timeSigNum, track.irregularGroups, track.irregularGroupCount);
  const ends = new Set();
  let pos = -1;
  for (const g of groups) {
    pos += g;
    if (pos < track.length - 1) ends.add(pos);
  }
  return ends;
}

function RenderTrack(trackIndex) {
  const track = tracks[trackIndex];
  if (!track) return;
  const container = document.getElementById(`track-${trackIndex}`);
  const gateContainer = document.getElementById(`gate-${trackIndex}`);
  if (!container || !gateContainer) return;
  container.innerHTML = '';
  gateContainer.innerHTML = '';

  const barEnds = BarEndIndices(track);

  for (let i = 0; i < track.length; i++) {
    const step = track.steps[i];
    const isRest = step.note === WIRE_REST;

    const isPanelFocus = panelPage === 1 && trackIndex === panelSelectedTrack && i === panelScrubStep;
    const cell = document.createElement('div');
    cell.className = 'step' + (isRest ? ' rest' : '') + (step.tied ? ' tied' : '') + (step.accent ? ' accent' : '') + (isPanelFocus ? ' panelFocus' : '');
    if (!isRest) {
      const bar = document.createElement('div');
      bar.className = 'pitchBar';
      bar.style.height = `${Math.round((step.note / 120) * 100)}%`;
      cell.appendChild(bar);
    }
    if (step.ratchetCount > 1) {
      const badge = document.createElement('div');
      badge.className = 'ratchetBadge';
      badge.textContent = step.ratchetCount;
      cell.appendChild(badge);
    }
    if (!isRest) {
      const noteLabel = document.createElement('div');
      noteLabel.className = 'noteLabel';
      noteLabel.textContent = NoteName(step.note);
      cell.appendChild(noteLabel);
    }
    const restZone = document.createElement('div');
    restZone.className = 'restZone';
    cell.appendChild(restZone);
    if (barEnds.has(i)) cell.classList.add('barEnd');
    container.appendChild(cell);

    const gateCell = document.createElement('div');
    gateCell.className = 'gateStep';
    const gateFill = document.createElement('div');
    gateFill.className = 'gateFill';
    gateFill.style.height = `${step.gateLenPct}%`;
    gateCell.appendChild(gateFill);
    if (barEnds.has(i)) gateCell.classList.add('barEnd');
    gateContainer.appendChild(gateCell);
  }

  // Playhead indicator: a single overlay element that moves via CSS
  // left/width transition (plan 4.3 - "animate the playhead outline's
  // position ... so it visibly steps rather than teleports"), rather than
  // toggling an outline class on a different cell each step - a CSS
  // transition can't animate "between" two different DOM elements, only
  // a single element's own property change, so one persistent overlay is
  // what makes the motion actually visible rather than an instant swap.
  const indicator = document.createElement('div');
  indicator.className = 'playheadIndicator';
  container.appendChild(indicator);
  PositionPlayhead(trackIndex, /*instant=*/true);

  RenderPlayheads();
}

// Time signature controls (plan 2.3/4.5): a mode toggle plus the input that
// matches it. Clicking a mode button only shows/hides the right input -
// it does NOT commit a mode change to firmware by itself, since Regular
// vs Irregular is a property of which message you send (MSG_SET_TIMESIG
// vs MSG_SET_TIMESIG_IRREGULAR), not a separate mode-set message. The
// firmware's actual mode only changes when you edit the matching input.
// Live panel-focus display: shows which page/track/step is currently
// selected on the physical panel, and (re-)renders whichever track(s)
// need their focus border updated - both the newly-focused track and
// whichever one held focus before, since focus can move between tracks
// or off the Middle page entirely (RenderTrack recomputes the border
// fresh each call from current panelPage/panelSelectedTrack/panelScrubStep,
// so re-rendering the old track correctly makes its border disappear too).
let lastPanelFocusTrack = -1;

function RenderPanelState() {
  const el = document.getElementById('panelStateLabel');
  if (el) {
    const pageName = PANEL_PAGE_NAMES[panelPage] || '?';
    const stepPart = panelPage === 1 ? ` · Step ${panelScrubStep + 1}` : '';
    el.textContent = `Panel: ${pageName} · Track ${panelSelectedTrack + 1}${stepPart}`;
  }
  if (tracks[panelSelectedTrack]) RenderTrack(panelSelectedTrack);
  // Only scroll on an actual track-focus CHANGE, not every render (this
  // function also fires on scrub-step changes within the same track,
  // which shouldn't yank the page around) - and only on Down/Middle,
  // since Up doesn't use Y for track-select (see the loop below). Opt-in
  // (checkbox defaults unchecked) - found via a user report that it could
  // scroll the panel-state label itself off-screen, awkward while actively
  // troubleshooting panel behavior and watching that label.
  if (lastPanelFocusTrack !== panelSelectedTrack && panelPage !== 2 && document.getElementById('autoScrollToFocus')?.checked) {
    document.getElementById(`trackBlock-${panelSelectedTrack}`)?.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
  }
  if (lastPanelFocusTrack !== -1 && lastPanelFocusTrack !== panelSelectedTrack && tracks[lastPanelFocusTrack]) {
    RenderTrack(lastPanelFocusTrack);
  }
  lastPanelFocusTrack = panelSelectedTrack;

  // Whole-track border+glow, on top of the per-step dashed focus border -
  // visible at a glance/from a distance (e.g. standing at a synth across
  // the room), not just up close on the exact scrubbed step. Up page
  // doesn't currently use Y for track-select (reserved), so the
  // highlight is hidden there rather than showing a stale/irrelevant track.
  for (let t = 0; t < numTracks; t++) {
    const trackDiv = document.getElementById(`track-${t}`)?.closest('.track');
    if (trackDiv) trackDiv.classList.toggle('panelFocusTrack', panelPage !== 2 && t === panelSelectedTrack);
  }
}

function RenderLengthControl(trackIndex) {
  const track = tracks[trackIndex];
  const el = document.getElementById(`length-${trackIndex}`);
  if (track && el) el.value = track.length;
}

// Scoped to `root` (default: the whole document) rather than always
// querying globally - milestone 13's dynamically-added track blocks call
// this scoped to just the new block, so re-running it for an existing
// track's already-populated selects doesn't append duplicate options.
function PopulateKeyScaleSelectors(root = document) {
  root.querySelectorAll('.keySelect').forEach(sel => {
    KEY_NAMES.forEach((name, i) => {
      const opt = document.createElement('option');
      opt.value = i;
      opt.textContent = name;
      sel.appendChild(opt);
    });
  });
  root.querySelectorAll('.scaleSelect').forEach(sel => {
    SCALE_NAMES.forEach((name, i) => {
      const opt = document.createElement('option');
      opt.value = i;
      opt.textContent = name;
      sel.appendChild(opt);
    });
    if (sel.classList.contains('defaultChromatic')) sel.value = 3;
  });
}

// ---------------------------------------------------------------------------
// Track growth (plan 4.9/4.10, milestone 13): tracks 0/1 are structural
// CV+Pulse (never removable - MSG_REMOVE_TRACK's firmware-side NACK already
// enforces this, this is just the matching UI affordance), tracks 2+ are
// MIDI-only, added/removed one at a time (MSG_ADD_TRACK always appends,
// MSG_REMOVE_TRACK always removes the highest index - no arbitrary-index
// add/remove, mirroring the firmware's own simplest-possible design).

function AddTrackUI() {
  SendSysEx(MSG_ADD_TRACK, []);
}

function RemoveTrackUI() {
  SendSysEx(MSG_REMOVE_TRACK, []);
}

function SetMidiChannelUI(trackIndex, value) {
  SendSysEx(MSG_SET_MIDI_CHANNEL, [trackIndex, parseInt(value, 10)]);
}

function ToggleMidiEnabledUI(trackIndex, checkbox) {
  SendSysEx(MSG_SET_MIDI_ENABLED, [trackIndex, checkbox.checked ? 1 : 0]);
}

// Builds the full per-track DOM block - same markup that used to be
// hand-duplicated twice (once per track) directly in index.html, now
// generated for however many tracks actually exist. RenderTrack/
// RenderTimeSigControls/RenderLengthControl/etc. were already written
// looking up elements by `${id}-${trackIndex}`, so they need no changes -
// only the markup's SOURCE moved from static HTML to this function.
function BuildTrackBlock(t) {
  const wrapper = document.createElement('div');
  wrapper.className = 'track';
  wrapper.id = `trackBlock-${t}`;
  const isCvTrack = t < NUM_CV_TRACKS;
  const badge = isCvTrack ? `CV${t + 1} + Pulse${t + 1}` : 'MIDI only';
  const badgeClass = isCvTrack ? 'trackBadge cvBadge' : 'trackBadge midiBadge';
  wrapper.innerHTML = `
    <h3>Track ${t + 1} <span class="${badgeClass}">${badge}</span>
      <button id="mute-${t}" class="muteBtn" onclick="ToggleMute(${t})">Mute</button>
      <button id="solo-${t}" class="soloBtn" onclick="ToggleSolo(${t})">Solo</button>
      <button class="lockBtn" onclick="ToggleTrackLock(${t}, this)">Lock</button>
    </h3>
    <div class="timeSigControls">
      length: <input type="number" id="length-${t}" value="8" min="1" max="64" size="3" onchange="SetLength(${t}, this.value)">
      <button id="timesig-mode-regular-${t}" class="timeSigModeBtn active" onclick="SetTimeSigModeUI(${t},'regular')">Regular</button>
      <button id="timesig-mode-irregular-${t}" class="timeSigModeBtn" onclick="SetTimeSigModeUI(${t},'irregular')">Irregular</button>
      <span id="timesig-regular-wrap-${t}">steps/bar: <input type="number" id="timesig-regular-${t}" value="4" min="1" max="32" size="3" onchange="SetTimeSig(${t}, this.value)"></span>
      <span id="timesig-irregular-wrap-${t}" style="display:none">groups: <input type="text" id="timesig-irregular-${t}" placeholder="3+3+2" size="8" onchange="SetTimeSigIrregularFromText(${t}, this.value)"></span>
      MIDI ch: <select id="midi-channel-${t}" onchange="SetMidiChannelUI(${t}, this.value)"></select>
      <label><input type="checkbox" id="midi-enabled-${t}" checked onchange="ToggleMidiEnabledUI(${t}, this)"> MIDI on</label>
    </div>
    <div class="scaleControls">
      <span>Scale (used for drawing assist and Snap to Scale):
        <select id="draw-key-${t}" class="keySelect" onchange="SetDrawScale(${t})"></select>
        <select id="draw-scale-${t}" class="scaleSelect defaultChromatic" onchange="SetDrawScale(${t})"></select>
        <button onclick="ApplySnapToScale(${t})">Snap existing notes to this scale</button>
      </span>
      <span>Live Shift:
        <input type="number" id="shift-${t}" value="0" min="-24" max="24" size="3" onchange="SetLiveShift(${t}, this.value)">
        <span id="shift-label-${t}">+0</span>
      </span>
      <span>Transpose Notes:
        <input type="number" id="transpose-${t}" value="0" min="-24" max="24" size="3">
        <button onclick="ApplyTransposeNotes(${t})">Apply</button>
      </span>
      <span>
        <button onclick="RandomizeTrackUI(${t})">Randomize</button>
        <span id="randomize-undo-${t}" class="randomizeUndo" style="display:none">Randomized - <a href="#" onclick="UndoRandomizeUI(${t}); return false;">Undo?</a></span>
      </span>
    </div>
    <div id="track-${t}" class="ribbon"></div>
    <div class="gateLabel">Gate length</div>
    <div id="gate-${t}" class="gateRibbon"></div>
  `;
  // MIDI channel options are plain numbers 1-16, unlike the key/scale
  // selects' named options - populated directly here rather than via
  // PopulateKeyScaleSelectors.
  const chSelect = wrapper.querySelector(`#midi-channel-${t}`);
  for (let ch = 1; ch <= 16; ch++) {
    const opt = document.createElement('option');
    opt.value = ch;
    opt.textContent = ch;
    chSelect.appendChild(opt);
  }
  PopulateKeyScaleSelectors(wrapper);
  return wrapper;
}

// Creates/destroys track DOM blocks to match `newCount`, with a brief
// fade/slide transition (plan 4.9) rather than an instant pop-in/out.
function SyncTrackBlocks(newCount) {
  const container = document.getElementById('tracksContainer');
  if (!container) return;

  for (let t = newCount; t < MAX_TRACKS; t++) {
    const el = document.getElementById(`trackBlock-${t}`);
    if (el) {
      attachedGestureTracks.delete(t); // re-attach gestures if this index is ever rebuilt
      el.classList.add('trackExit');
      setTimeout(() => el.remove(), 300);
    }
  }
  for (let t = 0; t < newCount; t++) {
    if (!document.getElementById(`trackBlock-${t}`)) {
      const block = BuildTrackBlock(t);
      block.classList.add('trackEnter');
      container.appendChild(block);
      // Double rAF: ensures the browser paints the "entering" state first,
      // so removing the class one frame later actually animates rather
      // than jumping straight to the end state.
      requestAnimationFrame(() => requestAnimationFrame(() => block.classList.remove('trackEnter')));
    }
  }

  const numLabel = document.getElementById('numTracksLabel');
  if (numLabel) numLabel.textContent = newCount;
  const maxLabel = document.getElementById('maxTracksLabel');
  if (maxLabel) maxLabel.textContent = MAX_TRACKS;
  // Mirrors the firmware's own boundary checks (MAX_TRACKS / NUM_CV_TRACKS)
  // as a UX nicety - the NACK already protects correctness either way.
  const addBtn = document.getElementById('addTrackBtn');
  if (addBtn) addBtn.disabled = newCount >= MAX_TRACKS;
  const removeBtn = document.getElementById('removeTrackBtn');
  if (removeBtn) removeBtn.disabled = newCount <= NUM_CV_TRACKS;
}

// Draw Scale: client-side only, see the `drawScale` declaration above.
function SetDrawScale(trackIndex) {
  const key = parseInt(document.getElementById(`draw-key-${trackIndex}`).value, 10);
  const scale = parseInt(document.getElementById(`draw-scale-${trackIndex}`).value, 10);
  if (scale === 3) delete drawScale[trackIndex]; // Chromatic = free drawing, no assist
  else drawScale[trackIndex] = { key, scale };
  // Also tells firmware, so the panel's Middle/Step-Edit pitch knob snaps
  // to the same scale, not just mouse-drawn pitches.
  SendSysEx(MSG_SET_DRAW_SCALE, [trackIndex, key, scale]);
}

// Snap to Scale: one-shot destructive rewrite of every non-rest step's
// stored note (plan 4.4) - NOT a real-time CV quantizer. Shares the same
// key/scale selector as Draw Scale (combined per user request - simpler
// than two near-identical selectors, even though it means you can no
// longer draw in one scale and snap to a different one without changing
// the selector in between). Applies the JS-mirrored algorithm locally for
// instant feedback (MSG_ACK carries no data, same reasoning as the
// time-signature fix earlier), then re-requests the track as an
// authoritative check.
function ApplySnapToScale(trackIndex) {
  const key = parseInt(document.getElementById(`draw-key-${trackIndex}`).value, 10);
  const scale = parseInt(document.getElementById(`draw-scale-${trackIndex}`).value, 10);
  const track = tracks[trackIndex];
  if (track) {
    for (const step of track.steps) {
      if (step.note !== WIRE_REST) step.note = NearestScaleNote(step.note, key, scale);
    }
    RenderTrack(trackIndex);
  }
  SendSysEx(MSG_SNAP_TO_SCALE, [trackIndex, key, scale]);
  RequestTrackSteps(trackIndex);
  DebugLog(`Snap to Scale: track ${trackIndex} -> ${KEY_NAMES[key]} ${SCALE_NAMES[scale]}`);
}

// Live Shift: non-destructive, output-time-only offset (plan 4.10) - does
// NOT rewrite steps[], so no local step data needs updating, just the
// wire-encoded value (shift+64, since SysEx data bytes must be 0-127).
// Mute/Solo (plan 4.10): per-track buttons + shared "is this track
// currently audible" dimming logic - solo flips the default, so muting
// one track's button literally being false doesn't mean it's audible if
// some OTHER track is soloed. Recomputed for every track whenever any
// track's live state changes, not just the one that changed.
function RenderLiveState(trackIndex, shift, muted, solo, midiChannel, midiEnabled) {
  const shiftInput = document.getElementById(`shift-${trackIndex}`);
  const shiftLabel = document.getElementById(`shift-label-${trackIndex}`);
  if (shiftInput) shiftInput.value = shift;
  if (shiftLabel) shiftLabel.textContent = (shift > 0 ? '+' : '') + shift;
  const muteBtn = document.getElementById(`mute-${trackIndex}`);
  const soloBtn = document.getElementById(`solo-${trackIndex}`);
  if (muteBtn) muteBtn.classList.toggle('active', muted);
  if (soloBtn) soloBtn.classList.toggle('active', solo);
  if (midiChannel !== undefined) {
    const chSelect = document.getElementById(`midi-channel-${trackIndex}`);
    if (chSelect) chSelect.value = midiChannel;
  }
  if (midiEnabled !== undefined) {
    const enabledBox = document.getElementById(`midi-enabled-${trackIndex}`);
    if (enabledBox) enabledBox.checked = midiEnabled;
  }
}

// Restores the Draw Scale selector + internal `drawScale` state from
// firmware's track.key/scale - called on connect/reload so the Web UI
// agrees with whatever scale the panel is actually snapping to, rather
// than silently resetting to Chromatic while firmware still remembers
// the real value.
function RenderDrawScale(trackIndex, key, scale) {
  const keySelect = document.getElementById(`draw-key-${trackIndex}`);
  const scaleSelect = document.getElementById(`draw-scale-${trackIndex}`);
  if (keySelect) keySelect.value = key;
  if (scaleSelect) scaleSelect.value = scale;
  if (scale === 3) delete drawScale[trackIndex];
  else drawScale[trackIndex] = { key, scale };
}

function IsTrackAudible(trackIndex) {
  if (globalMute) return false; // master override - silences everything, even a soloed track
  const track = tracks[trackIndex];
  if (!track) return true;
  const anySolo = Object.values(tracks).some(t => t && t.solo);
  if (track.muted) return false;
  if (anySolo && !track.solo) return false;
  return true;
}

function RenderAllAudibility() {
  for (let t = 0; t < numTracks; t++) {
    const trackDiv = document.getElementById(`track-${t}`)?.closest('.track');
    if (trackDiv) trackDiv.classList.toggle('dimmed', !IsTrackAudible(t));
  }
}

function ToggleMute(trackIndex) {
  const track = tracks[trackIndex];
  const newMuted = !(track && track.muted);
  SendSysEx(MSG_SET_TRACK_MUTE, [trackIndex, newMuted ? 1 : 0]);
}

function ToggleSolo(trackIndex) {
  const track = tracks[trackIndex];
  const newSolo = !(track && track.solo);
  SendSysEx(MSG_SET_TRACK_SOLO, [trackIndex, newSolo ? 1 : 0]);
}

// Master silence-all - overrides every track's mute/solo, including a
// soloed track (panic/safety control, plan feedback after milestone 8).
function ToggleGlobalMute() {
  SendSysEx(MSG_SET_GLOBAL_MUTE, [globalMute ? 0 : 1]);
}

// CV input routing for track and step selection. Allows an external CV
// (e.g. from an attenuverter) to replace the Y/X knobs for selecting which
// track or step the panel focuses on. Values: 0=off, 1=CV1, 2=CV2.
let cvTrackRoute = 0;
let cvStepRoute  = 0;

function SetCVRouting() {
  cvTrackRoute = parseInt(document.getElementById('cvTrackRoute')?.value ?? 0, 10);
  cvStepRoute  = parseInt(document.getElementById('cvStepRoute')?.value ?? 0, 10);
  SendSysEx(MSG_SET_CV_ROUTING, [cvTrackRoute, cvStepRoute]);
}

// Reflects firmware's current calibration values back into the UI.
function RenderCVRouting(trackMin, trackMax, stepMin, stepMax) {
  const sel = (id, val) => { const el = document.getElementById(id); if (el) el.value = val; };
  sel('cvTrackRoute', cvTrackRoute);
  sel('cvStepRoute',  cvStepRoute);
  const fmt = (lo, hi) => lo === 0 && hi === 4095 ? 'uncalibrated (full range)' : `${lo}–${hi}`;
  const tLabel = document.getElementById('cvTrackCalLabel');
  const sLabel = document.getElementById('cvStepCalLabel');
  if (tLabel) tLabel.textContent = `Track cal: ${fmt(trackMin, trackMax)}`;
  if (sLabel) sLabel.textContent = `Step cal: ${fmt(stepMin, stepMax)}`;
}

// Two-point calibration for CV routing. The user sets their CV source to the
// desired minimum position, clicks "Set Min", then moves to maximum and clicks
// "Set Max". The firmware stores these as calibration endpoints so the full
// available voltage range (not the full ±6V ADC range) maps to track 0 / N-1.
// `which`: 0=track, 1=step. `point`: 0=min, 1=max.
const cvCalCapture = { trackMin: null, trackMax: null, stepMin: null, stepMax: null };

function CaptureCalPoint(which, point) {
  // Ask the firmware to send its current CVVal as a calibration sample.
  // The firmware reads the live CV input and encodes it in the response.
  SendSysEx(MSG_SET_CV_ROUTING_CAL, [which | 0x40, point]); // 0x40 flag = "read only, don't store yet"
  // For now, prompt the user to read the raw CV value shown in the routing state:
  const label = which === 0 ? 'track' : 'step';
  const pname = point === 0 ? 'min' : 'max';
  DebugLog(`CV cal: capturing ${label} ${pname} — set your CV to the ${pname} position and the firmware will record it`);
}

function ApplyCVCal(which) {
  const min = which === 0 ? cvCalCapture.trackMin : cvCalCapture.stepMin;
  const max = which === 0 ? cvCalCapture.trackMax : cvCalCapture.stepMax;
  if (min === null || max === null) { DebugLog('CV cal: capture both min and max first'); return; }
  // Encode 0-4095 values as 7-bit lo/hi pairs
  const lo = v => v & 0x7F, hi = v => (v >> 7) & 0x7F;
  SendSysEx(MSG_SET_CV_ROUTING_CAL, [which, lo(min), hi(min), lo(max), hi(max)]);
}

function ResetCVCal(which) {
  const lo = v => v & 0x7F, hi = v => (v >> 7) & 0x7F;
  SendSysEx(MSG_SET_CV_ROUTING_CAL, [which, lo(0), hi(0), lo(4095), hi(4095)]);
}

function SetLiveShift(trackIndex, value) {
  const shift = Math.max(-24, Math.min(24, parseInt(value, 10) || 0));
  document.getElementById(`shift-${trackIndex}`).value = shift;
  const label = document.getElementById(`shift-label-${trackIndex}`);
  if (label) label.textContent = (shift > 0 ? '+' : '') + shift;
  SendSysEx(MSG_SET_TRACK_SHIFT, [trackIndex, shift + 64]);
}

// Generic mouse-wheel-nudge for every numeric input (length, time
// signature, Live Shift, Transpose Notes, and any added later) - clamps to
// the input's own min/max attributes, then fires a synthetic 'change'
// event so each field's existing onchange="..." handler runs unmodified.
// One delegated listener covers every <input type="number"> with zero
// per-field wiring, replacing the earlier one-off WheelLiveShift.
document.addEventListener('wheel', (event) => {
  const input = event.target;
  if (!(input instanceof HTMLInputElement) || input.type !== 'number') return;
  event.preventDefault();
  const step = parseFloat(input.step) || 1;
  const delta = event.deltaY > 0 ? -step : step;
  let value = (parseFloat(input.value) || 0) + delta;
  if (input.min !== '') value = Math.max(value, parseFloat(input.min));
  if (input.max !== '') value = Math.min(value, parseFloat(input.max));
  input.value = value;
  input.dispatchEvent(new Event('change'));
}, { passive: false });

// Transpose Notes: the DESTRUCTIVE counterpart to Live Shift - rewrites
// every non-rest step's stored note by a fixed number of semitones,
// rather than offsetting output only. Grouped in the UI next to Snap to
// Scale since both are one-shot edits to stored data (plan 4.10).
function ApplyTransposeNotes(trackIndex) {
  const input = document.getElementById(`transpose-${trackIndex}`);
  const semitones = Math.max(-24, Math.min(24, parseInt(input.value, 10) || 0));
  const track = tracks[trackIndex];
  if (track) {
    for (const step of track.steps) {
      if (step.note !== WIRE_REST) step.note = Math.max(0, Math.min(126, step.note + semitones));
    }
    RenderTrack(trackIndex);
  }
  SendSysEx(MSG_TRANSPOSE_NOTES, [trackIndex, semitones + 64]);
  RequestTrackSteps(trackIndex);
  DebugLog(`Transpose Notes: track ${trackIndex} by ${semitones} semitones`);
}

// Randomize (plan 4.6): the updated pattern arrives via the existing
// MSG_TRACK_STEPS send-on-change push (no need to request/optimistically
// render it here) - this just fires the command and shows a low-stakes
// "Randomized - Undo?" affordance that fades on its own after a few
// seconds, encouraging experimentation rather than treating it as a
// risky/destructive action.
const randomizeUndoTimers = {};

function RandomizeTrackUI(trackIndex) {
  SendSysEx(MSG_RANDOMIZE, [trackIndex]);
  ShowRandomizeUndo(trackIndex);
}

function UndoRandomizeUI(trackIndex) {
  SendSysEx(MSG_UNDO_RANDOMIZE, [trackIndex]);
  HideRandomizeUndo(trackIndex);
}

function ShowRandomizeUndo(trackIndex) {
  const el = document.getElementById(`randomize-undo-${trackIndex}`);
  if (!el) return;
  el.style.display = '';
  clearTimeout(randomizeUndoTimers[trackIndex]);
  randomizeUndoTimers[trackIndex] = setTimeout(() => HideRandomizeUndo(trackIndex), 6000);
}

function HideRandomizeUndo(trackIndex) {
  const el = document.getElementById(`randomize-undo-${trackIndex}`);
  if (el) el.style.display = 'none';
  clearTimeout(randomizeUndoTimers[trackIndex]);
}

// Pattern bank (plan 4.7, milestone 11): two rows of 8 slot buttons -
// clicking a Save-to button immediately saves there, clicking a Load-from
// button immediately loads from there (no separate "select then confirm"
// step - mirrors EvoSeq's proven interface.html convention). Loaded steps
// arrive via the existing MSG_TRACK_STEPS push, same as Randomize.
function BuildSlotButtons() {
  const save = document.getElementById('saveSlots');
  const load = document.getElementById('loadSlots');
  if (!save || !load) return;
  for (let s = 0; s < NUM_SAVE_SLOTS; s++) {
    // Displayed 1-8 for a human-friendly label; internal index/wire value
    // stays 0-7 to match the firmware protocol (MSG_SAVE_SLOT/MSG_LOAD_SLOT
    // slot argument, MSG_SLOT_BITMAP's bit position).
    const label = s + 1;
    const sb = document.createElement('button');
    sb.className = 'slot';
    sb.textContent = label;
    sb.id = `saveSlot${s}`;
    sb.title = `Save to slot ${label}`;
    sb.onclick = () => { SendSysEx(MSG_SAVE_SLOT, [s]); ShowPatternBankStatus(`Saving to slot ${label}...`); };
    save.appendChild(sb);

    const lb = document.createElement('button');
    lb.className = 'slot';
    lb.textContent = label;
    lb.id = `loadSlot${s}`;
    lb.title = `Load from slot ${label}`;
    lb.onclick = () => { SendSysEx(MSG_LOAD_SLOT, [s]); ShowPatternBankStatus(`Loading slot ${label}...`); };
    load.appendChild(lb);
  }
}

function RenderSlots() {
  for (let s = 0; s < NUM_SAVE_SLOTS; s++) {
    const used = (slotBitmap >> s) & 1;
    document.getElementById(`saveSlot${s}`)?.classList.toggle('used', !!used);
    document.getElementById(`loadSlot${s}`)?.classList.toggle('used', !!used);
    document.getElementById(`saveSlot${s}`)?.classList.toggle('active', s === activeSlot);
    document.getElementById(`loadSlot${s}`)?.classList.toggle('active', s === activeSlot);
  }
  if (activeSlot >= 0) ShowPatternBankStatus(`Slot ${activeSlot + 1} active`);
}

let patternBankStatusTimer = null;
function ShowPatternBankStatus(text) {
  const el = document.getElementById('patternBankStatus');
  if (!el) return;
  el.textContent = text;
  clearTimeout(patternBankStatusTimer);
  patternBankStatusTimer = setTimeout(() => { el.textContent = ''; }, 4000);
}

// ---------------------------------------------------------------------------
// Diagnostics event log (plan 3.4/4.8, milestone 12): a pull-model log,
// matching the rest of this protocol's "explicit request, detectable
// failure" design (plan section 3.3) rather than an always-on broadcast -
// only polls while the panel is actually expanded, per the plan's "Web UI
// polls this only while the diagnostics panel/tab is visible."

const DIAG_TYPE_NAMES = ['StateChange', 'PanelPageChange', 'MidiNoteOn', 'MidiNoteOff', 'SysExReceived', 'SysExRejected', 'FlashSaveLoad'];
const DIAG_TYPE_CATEGORY = { 0: 'state', 6: 'state', 1: 'hardware', 2: 'midi', 3: 'midi', 4: 'sysex', 5: 'sysex' };

let diagEvents = []; // capped ring on the JS side too, see OnDiagEventBatch
let diagSinceSeq = 0;
let diagLogVisible = false;
let diagPollTimer = null;
let diagLastSyncedAt = 0;

// Mirrors firmware's Encode32/Decode32 (usb_link.h) - uint32 split into 5
// 7-bit-safe bytes (5*7=35 bits, covers the full 32-bit range), since a raw
// multi-byte value could exceed the 0-127 SysEx-data-byte limit.
function Encode32(v) {
  return [(v >>> 28) & 0x7F, (v >>> 21) & 0x7F, (v >>> 14) & 0x7F, (v >>> 7) & 0x7F, v & 0x7F];
}
function Decode32(bytes, offset) {
  return ((bytes[offset] << 28) | (bytes[offset + 1] << 21) | (bytes[offset + 2] << 14) | (bytes[offset + 3] << 7) | bytes[offset + 4]) >>> 0;
}

function ToggleDiagLog() {
  diagLogVisible = !diagLogVisible;
  const panel = document.getElementById('diagLogPanel');
  const btn = document.getElementById('diagLogToggle');
  if (panel) panel.style.display = diagLogVisible ? '' : 'none';
  if (btn) btn.textContent = diagLogVisible ? 'Hide' : 'Show';
  if (diagLogVisible) {
    RequestDiagEvents();
    StartDiagAutoSync(); // respects the Auto-sync checkbox - see that function
  } else {
    StopDiagAutoSync(); // closing the panel always stops polling, regardless of the checkbox
  }
}

// Decoupled from Show/Hide: the panel can stay open with polling paused
// (e.g. to read a batch of events without the table reflowing under you),
// or closed (which always stops polling outright via ToggleDiagLog above).
function ToggleDiagAutoSync() {
  if (document.getElementById('diagAutoSync')?.checked) StartDiagAutoSync();
  else StopDiagAutoSync();
}

function StartDiagAutoSync() {
  clearInterval(diagPollTimer);
  diagPollTimer = null;
  if (!document.getElementById('diagAutoSync')?.checked) return;
  diagPollTimer = setInterval(RequestDiagEvents, 2000);
}

function StopDiagAutoSync() {
  clearInterval(diagPollTimer);
  diagPollTimer = null;
}

function RequestDiagEvents() {
  SendSysEx(MSG_REQUEST_DIAG_EVENTS, Encode32(diagSinceSeq));
}

// Manual recovery: if a single corrupted decode ever still slips through
// (e.g. a truncated send within the receiver's length cap - see
// MAX_SYSEX_BUF's comment), it can leave diagSinceSeq set to an
// implausibly large value that firmware's real seq counter will never
// catch up to, silently stalling all future polls. Clearing local state
// and refetching from seq 0 (= "from the oldest still-buffered event",
// per Diagnostics::Fetch in diagnostics.h) recovers without a page reload.
function ResyncDiagLog() {
  diagEvents = [];
  diagSinceSeq = 0;
  RenderDiagLog();
  RequestDiagEvents();
}

function OnDiagEventBatch(payload) {
  const count = payload[0];
  const more = payload[1] !== 0;
  let offset = 2;
  for (let i = 0; i < count; i++) {
    const type = payload[offset]; offset += 1;
    const seq = Decode32(payload, offset); offset += 5;
    const timestampMs = Decode32(payload, offset); offset += 5;
    const arg0 = payload[offset]; offset += 1;
    const arg1 = payload[offset]; offset += 1;
    diagEvents.push({ seq, timestampMs, type, arg0, arg1 });
    diagSinceSeq = seq;
    console.log(`[StepBridge diag] seq=${seq} t=${timestampMs}ms ${DIAG_TYPE_NAMES[type] ?? type} ${FormatDiagDetail(type, arg0, arg1)}`);
  }
  while (diagEvents.length > 500) diagEvents.shift();
  diagLastSyncedAt = Date.now();
  RenderDiagLog();
  if (more) RequestDiagEvents(); // catch up immediately rather than waiting for the next poll tick
}

function FormatDiagDetail(type, arg0, arg1) {
  switch (type) {
    case 0: { // StateChange
      const field = arg0 === 0 ? 'Transport' : 'Clock';
      const labels = arg0 === 0 ? ['Stopped', 'Paused', 'Playing'] : ['Internal', 'Ext Pulse', 'MIDI Clock'];
      return `${field} = ${labels[arg1] ?? arg1}`;
    }
    case 1: { // PanelPageChange
      const pages = ['Down', 'Middle', 'Up'];
      return `Page = ${pages[arg0] ?? arg0}`;
    }
    case 2: case 3: // MidiNoteOn/Off - arg0 is already just the channel nibble (0-15)
      return `ch=${arg0 + 1} note=${arg1}`;
    case 4: // SysExReceived
      return `cmd=0x${arg0.toString(16)}`;
    case 5: // SysExRejected
      return `cmd=0x${arg0.toString(16)} reason=${arg1}`;
    case 6: // FlashSaveLoad - +1 to match the 1-8 slot labels used elsewhere in the UI
      return `${arg0 === 0 ? 'Saved' : 'Loaded'} slot ${arg1 + 1}`;
    default:
      return `arg0=${arg0} arg1=${arg1}`;
  }
}

function RenderDiagLog() {
  const body = document.getElementById('diagLogBody');
  if (!body) return;
  const activeCategories = new Set(
    Array.from(document.querySelectorAll('.diagFilter:checked')).map((el) => el.value)
  );
  body.innerHTML = '';
  const visible = diagEvents.filter((e) => activeCategories.has(DIAG_TYPE_CATEGORY[e.type]));
  for (const e of visible.slice(-200)) {
    const tr = document.createElement('tr');
    tr.className = `diag-${DIAG_TYPE_NAMES[e.type]?.toLowerCase() ?? 'unknown'}`;
    const time = new Date(e.timestampMs).toISOString().substr(11, 8); // ms-since-boot, not wall clock - just a relative HH:MM:SS readout
    tr.innerHTML = `<td>${e.seq}</td><td>${time}</td><td>${DIAG_TYPE_NAMES[e.type] ?? e.type}</td><td>${FormatDiagDetail(e.type, e.arg0, e.arg1)}</td>`;
    body.appendChild(tr);
  }
  const syncedEl = document.getElementById('diagLogSynced');
  if (syncedEl) {
    syncedEl.textContent = diagLastSyncedAt ? `synced ${Math.round((Date.now() - diagLastSyncedAt) / 1000)}s ago` : 'never synced';
  }
}

// Found via a user report: the table repaints too often (every poll while
// actively glitching) to select-and-copy by hand - this takes a snapshot
// of the currently-filtered rows as plain text instead, so the repaint
// can't race the copy. Copies ALL matching events (not just the 200 most
// recent rows RenderDiagLog actually draws), since the full up-to-500
// buffer is a strictly better artifact for pasting elsewhere than what's
// merely visible on screen.
async function CopyDiagLog() {
  const activeCategories = new Set(
    Array.from(document.querySelectorAll('.diagFilter:checked')).map((el) => el.value)
  );
  const visible = diagEvents.filter((e) => activeCategories.has(DIAG_TYPE_CATEGORY[e.type]));
  const lines = visible.map((e) => {
    const time = new Date(e.timestampMs).toISOString().substr(11, 8);
    return `${e.seq}\t${time}\t${DIAG_TYPE_NAMES[e.type] ?? e.type}\t${FormatDiagDetail(e.type, e.arg0, e.arg1)}`;
  });
  const text = `seq\ttime\ttype\tdetail\n${lines.join('\n')}`;
  const btn = document.getElementById('diagLogCopyBtn');
  try {
    await navigator.clipboard.writeText(text);
    if (btn) { const orig = btn.textContent; btn.textContent = 'Copied!'; setTimeout(() => { btn.textContent = orig; }, 1500); }
  } catch (e) {
    // Clipboard API can fail (permissions, non-HTTPS context) - fall back
    // to a temporary off-screen textarea + execCommand, the older but more
    // widely-supported copy mechanism.
    const ta = document.createElement('textarea');
    ta.value = text;
    ta.style.position = 'fixed';
    ta.style.left = '-9999px';
    document.body.appendChild(ta);
    ta.select();
    try {
      document.execCommand('copy');
      if (btn) { const orig = btn.textContent; btn.textContent = 'Copied!'; setTimeout(() => { btn.textContent = orig; }, 1500); }
    } catch (e2) {
      DebugLog(`Copy to clipboard failed: ${e2}`);
    }
    document.body.removeChild(ta);
  }
}

setInterval(() => { if (diagLogVisible) RenderDiagLog(); }, 1000); // keeps the "synced Ns ago" readout ticking

// Click-to-preview (plan 4.3): fire-and-forget MIDI-only audition, rides
// along with every pitch-drag edit rather than needing a separate tap
// gesture - see AttachRibbonGestures' pitch branch.
function PreviewNote(trackIndex, note) {
  SendSysEx(MSG_PREVIEW_NOTE, [trackIndex, note]);
}

// Brief highlight on any step edit (plan 4.3, "Edit flash") - a quick
// "yes, that registered" pulse. Must run AFTER RenderTrack has rebuilt the
// DOM for this track, since the rebuild would otherwise wipe the class
// immediately.
function FlashEdit(trackIndex, stepIndex) {
  const container = document.getElementById(`track-${trackIndex}`);
  const cell = container && container.children[stepIndex];
  if (!cell) return;
  cell.classList.add('editFlash');
  setTimeout(() => cell.classList.remove('editFlash'), 300);
}

function RenderTimeSigControls(trackIndex) {
  const track = tracks[trackIndex];
  if (!track) return;
  const isIrregular = track.timeSigMode === 1;
  const regularBtn = document.getElementById(`timesig-mode-regular-${trackIndex}`);
  const irregularBtn = document.getElementById(`timesig-mode-irregular-${trackIndex}`);
  const regularWrap = document.getElementById(`timesig-regular-wrap-${trackIndex}`);
  const irregularWrap = document.getElementById(`timesig-irregular-wrap-${trackIndex}`);
  const regularInput = document.getElementById(`timesig-regular-${trackIndex}`);
  const irregularInput = document.getElementById(`timesig-irregular-${trackIndex}`);
  if (!regularBtn) return;
  regularBtn.classList.toggle('active', !isIrregular);
  irregularBtn.classList.toggle('active', isIrregular);
  if (regularWrap) regularWrap.style.display = isIrregular ? 'none' : '';
  if (irregularWrap) irregularWrap.style.display = isIrregular ? '' : 'none';
  if (regularInput) regularInput.value = track.timeSigNum;
  if (irregularInput) irregularInput.value = (track.irregularGroups || []).join('+');
}

function SetTimeSigModeUI(trackIndex, mode) {
  document.getElementById(`timesig-mode-regular-${trackIndex}`).classList.toggle('active', mode === 'regular');
  document.getElementById(`timesig-mode-irregular-${trackIndex}`).classList.toggle('active', mode === 'irregular');
  document.getElementById(`timesig-regular-wrap-${trackIndex}`).style.display = mode === 'regular' ? '' : 'none';
  document.getElementById(`timesig-irregular-wrap-${trackIndex}`).style.display = mode === 'irregular' ? '' : 'none';
}

// MSG_ACK carries no data (just the original command ID), so without an
// optimistic local update the ribbon would show stale dividers until a
// manual Refresh re-fetched MSG_TRACK_STEPS - confirmed missing during
// testing. Update the local copy and re-render immediately, same pattern
// already used for step edits elsewhere in this file.
function SetTimeSig(trackIndex, value) {
  const num = Math.max(1, Math.min(32, parseInt(value, 10) || 4));
  const track = tracks[trackIndex];
  if (track) {
    track.timeSigMode = 0;
    track.timeSigNum = num;
    RenderTrack(trackIndex);
  }
  SendSysEx(MSG_SET_TIMESIG, [trackIndex, num]);
}

function SetTimeSigIrregularFromText(trackIndex, text) {
  const groups = text.split('+').map(s => parseInt(s.trim(), 10)).filter(n => n >= 1 && n <= 32);
  if (groups.length < 1 || groups.length > 8) {
    DebugLog('Irregular time signature needs 1-8 groups of 1-32 steps each, e.g. "3+3+2"');
    return;
  }
  const track = tracks[trackIndex];
  if (track) {
    track.timeSigMode = 1;
    track.irregularGroupCount = groups.length;
    track.irregularGroups = groups;
    RenderTrack(trackIndex);
  }
  SendSysEx(MSG_SET_TIMESIG_IRREGULAR, [trackIndex, groups.length, ...groups]);
}

// Length is trickier than time signature to predict locally: GrowLength's
// replicate-last-step / reveal-old-data-on-regrow rule (plan 2.3) depends
// on highWaterLength, which the Web UI doesn't track. Shrinking is safe to
// guess (just render fewer steps); growing isn't, since we don't know
// whether firmware will replicate the last step or reveal previously-
// hidden data. So: render an immediate best-effort guess (replicate, the
// more common case) for instant feedback, then re-request the track to
// pick up whatever firmware actually did.
function SetLength(trackIndex, value) {
  const track = tracks[trackIndex];
  if (!track) return;
  const newLength = Math.max(1, Math.min(64, parseInt(value, 10) || track.length));
  if (newLength > track.length) {
    const last = track.steps[track.length - 1];
    for (let i = track.length; i < newLength; i++) track.steps[i] = Object.assign({}, last);
  }
  track.length = newLength;
  RenderTrack(trackIndex);
  SendSysEx(MSG_SET_LENGTH, [trackIndex, newLength]);
  RequestTrackSteps(trackIndex);
}

// Moves the per-track playhead overlay to the current step. `instant`
// suppresses the CSS transition for the very first positioning after a
// RenderTrack rebuild (a freshly-appended element has no prior position to
// animate FROM, so without this it would visibly slide in from the left
// edge every time you edit a step) - re-enabled one frame later so the
// NEXT real step advance animates normally.
function PositionPlayhead(trackIndex, instant) {
  const track = tracks[trackIndex];
  const container = document.getElementById(`track-${trackIndex}`);
  const indicator = container && container.querySelector('.playheadIndicator');
  if (!track || !indicator) return;
  // Pixel-accurate via the actual cell's own box, not percentage math -
  // the ribbon is a flex row with a gap, so a plain 100/length percentage
  // wouldn't line up with real cell edges.
  //
  // Positioned at the RIGHT edge of the current step (not centered, and
  // not the left edge either - both tried first). The gate/accent flash
  // fires instantly on arrival, but the line's CSS transition takes ~80ms
  // to glide into place, so a left-edge line visibly lagged behind the
  // already-glowing cell. Landing at the right edge means the line
  // arrives roughly where the NEXT step is about to start, reading as
  // "leading into" rather than "catching up to" the active step.
  const cell = container.children[Math.max(0, track.currentStep)];
  if (!cell || cell === indicator) return;
  const left = cell.offsetLeft + cell.offsetWidth;
  if (instant) {
    indicator.style.transition = 'none';
    indicator.style.left = `${left}px`;
    requestAnimationFrame(() => { indicator.style.transition = ''; });
  } else {
    indicator.style.left = `${left}px`;
  }
}

const lastRenderedStep = {}; // trackIndex -> step index, to detect arrival at a NEW step

function RenderPlayheads() {
  for (let t = 0; t < numTracks; t++) {
    const track = tracks[t];
    if (!track) continue;
    PositionPlayhead(t, false);

    const arrived = lastRenderedStep[t] !== track.currentStep;
    lastRenderedStep[t] = track.currentStep;
    if (!arrived) continue;

    // Gate-progress / accent flash (plan 4.3): approximates real gate
    // timing from measured BPM and this step's gateLenPct, since we only
    // get coarse step-level playhead updates over SysEx, not per-sample
    // gate state - a deliberate simplification, not true gate tracking.
    // Ratchet sub-pulses are NOT flashed individually for the same reason
    // (no sub-step timing available client-side); a ratcheted step still
    // gets one flash like any other, noted here as a known limitation
    // rather than attempted and faked.
    const step = track.steps[track.currentStep];
    if (!step || step.note === WIRE_REST) continue;
    const container = document.getElementById(`track-${t}`);
    const cell = container && container.children[track.currentStep];
    if (!cell) continue;
    const stepDurationMs = currentBpm > 0 ? 60000 / currentBpm : 500;
    const flashMs = Math.max(50, Math.min(2000, stepDurationMs * (step.gateLenPct / 100)));
    const flashClass = step.accent ? 'accentFlash' : 'gateFlash';
    cell.style.setProperty('--gateFlashDuration', `${flashMs}ms`);
    cell.classList.remove('gateFlash', 'accentFlash'); // restart the animation even if the previous one is still running
    void cell.offsetWidth; // force reflow so re-adding the class below actually restarts the CSS animation
    cell.classList.add(flashClass);
    setTimeout(() => cell.classList.remove(flashClass), flashMs);
  }
}

function AttachRibbonGestures(container, trackIndex) {
  if (!container) return;
  let dragging = false;
  let dragMode = null; // 'pitch' | 'rest' | 'tie' | 'accent'
  let dragTargetValue = null; // fixed boolean target for rest/tie/accent paint gestures

  function ModeForEvent(e) {
    if (e.shiftKey) return 'rest';
    if (e.ctrlKey || e.metaKey) return 'tie';
    if (e.altKey) return 'accent';
    return 'pitch';
  }

  function Apply(clientX, clientY) {
    if (IsEditLocked(trackIndex)) return;
    const idx = StepIndexAt(container, trackIndex, clientX);
    if (idx < 0) return;
    const track = tracks[trackIndex];
    const step = track.steps[idx];
    const cell = container.children[idx];
    const rect = (cell || container).getBoundingClientRect();

    if (dragMode === 'pitch') {
      const relY = 1 - (clientY - rect.top) / rect.height;
      let note = relY < REST_BAND ? WIRE_REST : Math.max(0, Math.min(120, Math.round(((relY - REST_BAND) / (1 - REST_BAND)) * 120)));
      // Draw Scale (plan 4.4) - client-side only input assist: snaps the
      // drawn pitch to the nearest note in the selected scale instead of
      // free chromatic. Independent of (and doesn't constrain) whatever
      // scale Snap to Scale later targets.
      if (note !== WIRE_REST) {
        const ds = drawScale[trackIndex];
        if (ds && ds.scale !== 3) note = NearestScaleNote(note, ds.key, ds.scale);
      }
      if (step.note === note) return;
      step.note = note;
      SendSysEx(MSG_SET_STEP, [trackIndex, idx, note]);
      // Preview rides along with every pitch change rather than being a
      // separate tap-only gesture (plain click already sets pitch here,
      // per the existing modifier scheme) - you hear what you draw as you
      // draw it, which is the "make it interactive" goal plan 4.3 asks
      // for, without needing a tap-vs-sweep gesture distinction.
      if (note !== WIRE_REST) PreviewNote(trackIndex, note);
    } else if (dragMode === 'rest') {
      if (dragTargetValue === null) dragTargetValue = step.note !== WIRE_REST; // first touched cell decides: make-rest or un-rest
      const newNote = dragTargetValue ? WIRE_REST : (step.lastNote || 60);
      if (step.note === newNote) return;
      if (step.note !== WIRE_REST) step.lastNote = step.note;
      step.note = newNote;
      SendSysEx(MSG_SET_STEP, [trackIndex, idx, newNote]);
    } else if (dragMode === 'tie') {
      if (dragTargetValue === null) dragTargetValue = !step.tied;
      if (step.tied === dragTargetValue) return;
      step.tied = dragTargetValue;
      if (step.tied) step.ratchetCount = 1; // mutually exclusive, plan 2.3
      SendSysEx(MSG_SET_STEP_TIE, [trackIndex, idx, step.tied ? 1 : 0]);
    } else if (dragMode === 'accent') {
      if (step.note === WIRE_REST) return; // accent on a rest is meaningless, plan 2.3
      if (dragTargetValue === null) dragTargetValue = !step.accent;
      if (step.accent === dragTargetValue) return;
      step.accent = dragTargetValue;
      SendSysEx(MSG_SET_STEP_ACCENT, [trackIndex, idx, step.accent ? 1 : 0]);
    }
    RenderTrack(trackIndex);
    // FlashEdit runs AFTER RenderTrack rebuilds the DOM - adding the class
    // before the rebuild would just get wiped immediately by innerHTML='' .
    FlashEdit(trackIndex, idx);
  }

  container.addEventListener('mousedown', (e) => {
    if (e.button !== 0) return;
    dragging = true;
    dragMode = ModeForEvent(e);
    dragTargetValue = null;
    Apply(e.clientX, e.clientY);
    e.preventDefault();
  });
  window.addEventListener('mousemove', (e) => { if (dragging) Apply(e.clientX, e.clientY); });
  window.addEventListener('mouseup', () => { dragging = false; dragMode = null; dragTargetValue = null; });

  container.addEventListener('wheel', (e) => {
    if (IsEditLocked(trackIndex)) return;
    e.preventDefault();
    const idx = StepIndexAt(container, trackIndex, e.clientX);
    if (idx < 0) return;
    const track = tracks[trackIndex];
    const step = track.steps[idx];
    const dir = e.deltaY > 0 ? -1 : 1;

    if (e.shiftKey) {
      const rc = Math.max(1, Math.min(8, step.ratchetCount + dir));
      if (rc === step.ratchetCount) return;
      step.ratchetCount = rc;
      if (rc > 1) step.tied = false; // mutually exclusive, plan 2.3
      SendSysEx(MSG_SET_STEP_RATCHET, [trackIndex, idx, rc]);
    } else {
      if (step.note === WIRE_REST) return;
      const ds = drawScale[trackIndex];
      const note = (ds && ds.scale !== 3)
        ? Math.max(0, Math.min(120, NextScaleDegree(step.note, ds.key, ds.scale, dir)))
        : Math.max(0, Math.min(120, step.note + dir));
      if (note === step.note) return;
      step.note = note;
      SendSysEx(MSG_SET_STEP, [trackIndex, idx, note]);
    }
    RenderTrack(trackIndex);
  }, { passive: false });
}

function AttachGateRibbonGestures(container, trackIndex) {
  if (!container) return;
  let dragging = false;

  function Apply(clientX, clientY) {
    if (IsEditLocked(trackIndex)) return;
    const idx = StepIndexAt(container, trackIndex, clientX);
    if (idx < 0) return;
    const track = tracks[trackIndex];
    const step = track.steps[idx];
    const cell = container.children[idx];
    const rect = (cell || container).getBoundingClientRect();
    const rel = 1 - (clientY - rect.top) / rect.height;
    const pct = Math.max(1, Math.min(100, Math.round(rel * 100)));
    if (pct === step.gateLenPct) return;
    step.gateLenPct = pct;
    SendSysEx(MSG_SET_STEP_GATE, [trackIndex, idx, pct]);
    RenderTrack(trackIndex);
  }

  container.addEventListener('mousedown', (e) => {
    if (e.button !== 0) return;
    dragging = true;
    Apply(e.clientX, e.clientY);
    e.preventDefault();
  });
  window.addEventListener('mousemove', (e) => { if (dragging) Apply(e.clientX, e.clientY); });
  window.addEventListener('mouseup', () => { dragging = false; });

  container.addEventListener('wheel', (e) => {
    if (IsEditLocked(trackIndex)) return;
    e.preventDefault();
    const idx = StepIndexAt(container, trackIndex, e.clientX);
    if (idx < 0) return;
    const track = tracks[trackIndex];
    const step = track.steps[idx];
    const dir = e.deltaY > 0 ? -5 : 5;
    const pct = Math.max(1, Math.min(100, step.gateLenPct + dir));
    if (pct === step.gateLenPct) return;
    step.gateLenPct = pct;
    SendSysEx(MSG_SET_STEP_GATE, [trackIndex, idx, pct]);
    RenderTrack(trackIndex);
  }, { passive: false });
}

// ---------------------------------------------------------------------------
// Transport

let lastTransportState = 2; // Playing, matches firmware's boot default
let isArmed = false;

// "Armed" is not a transport state of its own (Stopped/Paused/Playing is
// unchanged) - it's a brief sub-state of Playing, true from the moment Play
// is pressed (with an external clock source) until the first real pulse/
// tick actually arrives. Pressing Play arms the sequencer rather than
// guaranteeing an instant start - showing this explicitly is what makes
// that momentary gap read as intentional rather than as nothing having
// happened yet (see the User Guide tab).
function RenderTransport(state) {
  document.querySelectorAll('.transportBtn').forEach((btn, i) => {
    btn.classList.toggle('active', i === state);
  });
  const el = document.getElementById('transportLabel');
  const armedNow = (state === 2 && isArmed);
  if (el)
  {
    el.textContent = armedNow ? 'Armed…' : (TRANSPORT_NAMES[state] || '?');
    el.classList.toggle('armed', armedNow);
  }
}

function SetTransport(state) {
  SendSysEx(MSG_SET_TRANSPORT, [state]);
}

// Mirrors RenderTransport/SetTransport. When the source is anything other
// than Internal, tempo is being driven externally - the BPM field becomes
// a read-only "following" indicator rather than an editable control, so
// it's never ambiguous whether you're setting tempo or just watching it
// (plan section 4.10).
function RenderClockSource(source) {
  document.querySelectorAll('.clockSourceBtn').forEach((btn, i) => {
    btn.classList.toggle('active', i === source);
  });
  const el = document.getElementById('clockSourceLabel');
  if (el) el.textContent = CLOCK_SOURCE_NAMES[source] || '?';
}

function SetClockSource(source) {
  SendSysEx(MSG_SET_CLOCK_SOURCE, [source]);
}

// This is a MEASURED value (actual timing between step advances), not a
// dial-in setting - there is no SetTempo to go with it. That's deliberate:
// it's the only way to see what's actually arriving from an external clock
// (e.g. confirming a Eurorack clock module's pulses-per-beat is correct),
// and it stays meaningful regardless of which clock source is selected.
let currentBpm = 0; // used by RenderPlayheads to estimate gate-flash duration

function RenderTempo(bpm) {
  currentBpm = bpm;
  const el = document.getElementById('tempoLabel');
  if (el) el.textContent = bpm > 0 ? `${bpm} BPM (measured)` : '-- BPM';
}

// ---------------------------------------------------------------------------
// Tabs

function ShowTab(name) {
  document.querySelectorAll('.tabPane').forEach(p => p.classList.toggle('active', p.id === 'tab-' + name));
  document.querySelectorAll('.tabBtn').forEach(b => b.classList.toggle('active', b.dataset.tab === name));
}

window.addEventListener('DOMContentLoaded', () => {
  PopulateKeyScaleSelectors();
  BuildSlotButtons();
  AcquireMidiAccess();
});
