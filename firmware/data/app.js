(() => {
  "use strict";

  const PARAM_META = {
    period_ms: { label: "Period (ms)", min: 100, max: 60000, step: 50, default: 2000 },
    amplitude_deg: { label: "Amplitude (°)", min: 0, max: 135, step: 0.5, default: 45 },
    offset_deg: { label: "Offset / Center (°)", min: 0, max: 270, step: 0.5, default: 135 },
    duty_pct: { label: "Duty (%)", min: 0, max: 100, step: 1, default: 50 },
    rise_pct: { label: "Rise (%)", min: 0, max: 100, step: 1, default: 25 },
    hold_pct: { label: "Hold (%)", min: 0, max: 100, step: 1, default: 25 },
    fall_pct: { label: "Fall (%)", min: 0, max: 100, step: 1, default: 25 },
    interval_min_ms: { label: "Min interval (ms)", min: 100, max: 600000, step: 100, default: 1500 },
    interval_max_ms: { label: "Max interval (ms)", min: 100, max: 600000, step: 100, default: 6000 },
    max_speed_dps: { label: "Max speed (\u00b0/s)", min: 5, max: 400, step: 5, default: 90 },
  };

  // Servo travel as calibrated in Settings — the jog fader's range, the
  // recording trace's vertical scale, and the angle-valued pattern parameters
  // all follow it, so a servo set up for 180° never shows a 270° control.
  let servoLimits = { min: 0, max: 270, center: 135 };

  // Must match FIRMWARE_VERSION in firmware/include/Config.h. These two ship
  // as separate uploads (`pio run -t upload` vs `-t uploadfs`), so one can
  // silently be older than the other — which looks exactly like "the feature
  // doesn't work" rather than "half of it isn't on the board". Comparing them
  // at runtime turns that into a visible banner instead of a hunt.
  const UI_VERSION = "2.1.0";

  let patternCatalog = []; // [{type,label,params:[...]}]
  let ws = null;
  let lastStatus = null;
  let recTrace = []; // {t, angle} while recording, relative ms
  let isMaster = false;
  let knownNodes = []; // [{id,angle,age_ms}] from /api/network/nodes
  let targetAllMode = true;
  let selectedTargets = new Set(); // checked node ids (used when targetAllMode is false)
  let extraTargetNodes = new Set(); // manually-added ids not (yet) in knownNodes

  // ---------- tiny helpers ----------
  const $ = (id) => document.getElementById(id);
  const clamp = (v, lo, hi) => (Number.isFinite(v) ? Math.min(hi, Math.max(lo, v)) : lo);

  async function apiGet(path) {
    const res = await fetch(path);
    return res.json();
  }
  async function apiPost(path, body) {
    const res = await fetch(path, {
      method: "POST",
      headers: body ? { "Content-Type": "application/json" } : undefined,
      body: body ? JSON.stringify(body) : undefined,
    });
    return res.json().catch(() => ({}));
  }

  // ---------- tabs ----------
  function initTabs() {
    document.querySelectorAll(".tabbtn").forEach((btn) => {
      btn.addEventListener("click", () => {
        document.querySelectorAll(".tabbtn").forEach((b) => b.classList.remove("active"));
        document.querySelectorAll(".tab").forEach((t) => t.classList.remove("active"));
        btn.classList.add("active");
        $(btn.dataset.tab).classList.add("active");
        if (btn.dataset.tab === "tab-settings") loadSettings();
      });
    });
  }

  // ---------- websocket / status ----------
  function connectWs() {
    const proto = location.protocol === "https:" ? "wss:" : "ws:";
    ws = new WebSocket(`${proto}//${location.host}/ws`);
    ws.onopen = () => $("wsDot").classList.replace("dot-off", "dot-on");
    ws.onclose = () => {
      $("wsDot").classList.replace("dot-on", "dot-off");
      setTimeout(connectWs, 1500);
    };
    ws.onerror = () => ws.close();
    ws.onmessage = (evt) => {
      try {
        const msg = JSON.parse(evt.data);
        if (msg.type === "status") applyStatus(msg);
      } catch (e) { /* ignore malformed frame */ }
    };
  }

  function applyStatus(s) {
    lastStatus = s;
    $("modeLabel").textContent = s.mode;
    if (s.firmware_version) $("fwVersion").textContent = s.firmware_version;
    checkVersionMatch(s.firmware_version);
    $("angleLabel").textContent = `${s.angle.toFixed(1)}°`;

    const recActive = s.recording && s.recording.active;
    $("recDot").classList.toggle("dot-on", !!recActive);
    $("recDot").classList.toggle("dot-off", !recActive);
    $("recLabel").textContent = recActive ? "recording" : "idle";
    $("recPoints").textContent = `${s.recording ? s.recording.points : 0} pts`;

    if (recActive) {
      recTrace.push({ t: s.uptime_ms, angle: s.angle, relay: !!s.relay_on });
      if (recTrace.length > 400) recTrace.shift();
      drawTrace();
    } else if (recTrace.length && s.recording && s.recording.points === 0) {
      recTrace = [];
      drawTrace();
    }

    // Keep the jog slider in sync when the servo is being driven by
    // something other than this browser (pattern/sequence/another client).
    if (document.activeElement !== $("jogSlider")) {
      $("jogSlider").value = s.angle;
      $("jogValue").textContent = s.angle.toFixed(1);
    }

    // Same for the relay: sequence playback and other clients switch it too.
    //
    // Deliberately NOT guarded on document.activeElement the way the jog
    // slider above is. A checkbox keeps focus after you click it, so that
    // guard would suppress every later update for the rest of the session —
    // the switch would sit wherever you last left it and never follow
    // sequence playback. What it does need is a brief echo window after a
    // local toggle, so an in-flight status frame sent before the command
    // landed can't visibly bounce the switch back.
    if (typeof s.relay_on === "boolean" && Date.now() >= relayEchoUntil) {
      $("relayToggle").checked = s.relay_on;
    }
  }

  // The light gets its own lane along the bottom rather than being drawn into
  // the angle plot: it's a two-state track, so a solid bar reads instantly
  // where a second line would just look like a flat trace. The faint wash
  // above it ties each lit stretch back to the motion it happened during.
  const TRACE_LANE_H = 14; // px of canvas height reserved for the light lane
  const TRACE_LANE_GAP = 4;

  function checkVersionMatch(fwVersion) {
    const banner = $("verWarn");
    if (!banner) return;
    // A board that predates the version field at all reports nothing, which is
    // itself a mismatch worth shouting about.
    const fw = fwVersion || "older than 2.1.0";
    if (fwVersion === UI_VERSION) {
      banner.style.display = "none";
      return;
    }
    banner.style.display = "";
    banner.innerHTML =
      `<strong>Firmware / web UI mismatch.</strong> This board runs firmware <code>${fw}</code> ` +
      `but these web UI files are <code>${UI_VERSION}</code>. They upload separately, so features ` +
      `that need both (light switch, random pattern) won't work until the firmware is updated too:` +
      `<br><code>pio run -e &lt;env&gt; -t upload</code> &nbsp;then&nbsp; <code>pio run -e &lt;env&gt; -t uploadfs</code>`;
  }

  function drawTrace() {
    const canvas = $("recCanvas");
    const ctx = canvas.getContext("2d");
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    if (recTrace.length < 2) return;

    const tMin = recTrace[0].t;
    const tMax = recTrace[recTrace.length - 1].t;
    const tSpan = Math.max(1, tMax - tMin);
    const laneTop = canvas.height - TRACE_LANE_H;
    const plotH = laneTop - TRACE_LANE_GAP;
    const xAt = (p) => ((p.t - tMin) / tSpan) * canvas.width;

    // Empty lane track, so "light never on" still looks deliberate.
    ctx.fillStyle = "#262b32";
    ctx.fillRect(0, laneTop, canvas.width, TRACE_LANE_H);

    // Coalesce consecutive lit samples into one bar each. Drawing a rect per
    // sample instead leaves hairline seams where subpixel edges land, which
    // reads as a dashed lane rather than a solid one (measured: 8 broken runs
    // across two lit stretches). Each sample holds its state until the next —
    // the same step semantics playback uses (SequenceStore::relayAtTime).
    const n = recTrace.length;
    let i = 0;
    while (i < n) {
      if (!recTrace[i].relay) { i++; continue; }
      let j = i;
      while (j + 1 < n && recTrace[j + 1].relay) j++;
      const x0 = xAt(recTrace[i]);
      const x1 = xAt(recTrace[Math.min(j + 1, n - 1)]);
      const w = Math.max(1, x1 - x0);
      ctx.fillStyle = "#e0a934";
      ctx.fillRect(x0, laneTop, w, TRACE_LANE_H);
      ctx.fillStyle = "rgba(224, 169, 52, 0.10)";
      ctx.fillRect(x0, 0, w, plotH);
      i = j + 1;
    }

    const span = Math.max(1, servoLimits.max - servoLimits.min);
    ctx.strokeStyle = "#4da3ff";
    ctx.lineWidth = 2;
    ctx.beginPath();
    recTrace.forEach((p, i) => {
      const x = xAt(p);
      const y = plotH - ((p.angle - servoLimits.min) / span) * plotH;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    ctx.stroke();
  }

  // ---------- jog ----------
  let lastJogSentAt = 0;
  function sendJog(angle) {
    const now = Date.now();
    if (now - lastJogSentAt < 40) return; // ~25Hz cap
    lastJogSentAt = now;
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ cmd: "jog", angle }));
    } else {
      apiPost("/api/manual/jog", { angle_deg: angle });
    }
  }

  function initJog() {
    const slider = $("jogSlider");
    slider.addEventListener("input", () => {
      const v = parseFloat(slider.value);
      $("jogValue").textContent = v.toFixed(1);
      sendJog(v);
    });
  }

  // ---------- relay / light ----------
  // Ignore incoming relay state until this timestamp: covers the round trip
  // of a toggle we just sent, so the switch doesn't flicker back on a status
  // frame that was already in flight. Comfortably longer than the ~100ms
  // status broadcast interval.
  let relayEchoUntil = 0;

  function sendRelay(on) {
    relayEchoUntil = Date.now() + 500;
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ cmd: "relay", on }));
    } else {
      apiPost("/api/relay", { on });
    }
  }

  function initRelay() {
    $("relayToggle").addEventListener("change", () => sendRelay($("relayToggle").checked));
  }

  // ---------- servo range (jog fader + angle-valued pattern params) ----------
  function applyServoLimits(servo) {
    if (!servo) return;
    const min = Number(servo.min_angle);
    const max = Number(servo.max_angle);
    if (!Number.isFinite(min) || !Number.isFinite(max) || max <= min) return;

    servoLimits = {
      min,
      max,
      center: Number.isFinite(Number(servo.center_angle)) ? Number(servo.center_angle) : (min + max) / 2,
    };

    const slider = $("jogSlider");
    slider.min = min;
    slider.max = max;
    // A 0.5° step is a sensible resolution for a 270° servo but coarse for a
    // small range, so scale it to keep roughly the same number of steps.
    slider.step = (max - min) >= 180 ? 0.5 : 0.1;
    // Until the first status frame lands, the slider has nothing real to show,
    // so start it at the configured center rather than at the markup's default
    // (which is a 270° servo's center and means nothing on a 180° one).
    slider.value = lastStatus ? clamp(parseFloat(slider.value), min, max) : servoLimits.center;
    $("jogValue").textContent = parseFloat(slider.value).toFixed(1);
    $("jogRangeHint").textContent = `Range ${min.toFixed(0)}\u2013${max.toFixed(0)}\u00b0 (from the servo calibration in Settings).`;

    // Keep the pattern forms' angle inputs inside the same travel.
    PARAM_META.offset_deg.min = min;
    PARAM_META.offset_deg.max = max;
    PARAM_META.offset_deg.default = servoLimits.center;
    PARAM_META.amplitude_deg.max = (max - min) / 2;
    PARAM_META.amplitude_deg.default = Math.min(45, (max - min) / 2);

    drawTrace();
  }

  // ---------- dynamic pattern-param forms ----------
  function buildParamForm(container, keys, idPrefix, values) {
    container.innerHTML = "";
    keys.forEach((key) => {
      const meta = PARAM_META[key];
      if (!meta) return;
      const label = document.createElement("label");
      label.className = "field";
      const span = document.createElement("span");
      span.textContent = meta.label;
      const input = document.createElement("input");
      input.type = "number";
      input.min = meta.min;
      input.max = meta.max;
      input.step = meta.step;
      input.id = `${idPrefix}_${key}`;
      input.value = values && values[key] !== undefined ? values[key] : meta.default;
      label.appendChild(span);
      label.appendChild(input);
      container.appendChild(label);
    });
  }

  function readParamForm(keys, idPrefix, type) {
    const out = { type };
    keys.forEach((key) => {
      const el = $(`${idPrefix}_${key}`);
      if (el) out[key] = parseFloat(el.value);
    });
    return out;
  }

  function catalogEntry(type) {
    return patternCatalog.find((e) => e.type === type) || patternCatalog[0];
  }

  function populatePatternSelect(selectEl) {
    selectEl.innerHTML = "";
    patternCatalog.forEach((e) => {
      const opt = document.createElement("option");
      opt.value = e.type;
      opt.textContent = e.label;
      selectEl.appendChild(opt);
    });
  }

  async function loadPatterns() {
    patternCatalog = await apiGet("/api/patterns");
    populatePatternSelect($("patternType"));
    populatePatternSelect($("asPatternType"));
    onPatternTypeChange();
    onAsPatternTypeChange();
  }

  function onPatternTypeChange() {
    const entry = catalogEntry($("patternType").value);
    if (!entry) return;
    buildParamForm($("patternParams"), entry.params.split(","), "pp");
  }

  function onAsPatternTypeChange() {
    const entry = catalogEntry($("asPatternType").value);
    if (!entry) return;
    buildParamForm($("asPatternParams"), entry.params.split(","), "ap");
  }

  // ---------- manual tab actions ----------
  function initPatternControls() {
    $("patternType").addEventListener("change", onPatternTypeChange);
    $("patternStart").addEventListener("click", () => {
      const entry = catalogEntry($("patternType").value);
      const params = readParamForm(entry.params.split(","), "pp", entry.type);
      apiPost("/api/pattern/start", params);
    });
    $("patternStop").addEventListener("click", () => apiPost("/api/pattern/stop"));
  }

  // ---------- record tab actions ----------
  function initRecordControls() {
    $("recStart").addEventListener("click", () => {
      recTrace = [];
      apiPost("/api/record/start");
    });
    $("recStop").addEventListener("click", () => apiPost("/api/record/stop"));
    $("recSave").addEventListener("click", () => {
      const name = $("recName").value.trim();
      if (!name) {
        alert("Enter a name for this sequence first.");
        return;
      }
      apiPost("/api/record/save", { name }).then((res) => {
        if (res && res.ok) {
          $("recName").value = "";
          refreshSequenceList();
        }
      });
    });
    $("recDiscard").addEventListener("click", () => {
      recTrace = [];
      drawTrace();
      apiPost("/api/record/discard");
    });
    $("seqStop").addEventListener("click", () => apiPost("/api/sequence/stop"));
    $("seqClearAll").addEventListener("click", () => {
      if (!sequenceCatalog.length) return;
      if (confirm(`Delete all ${sequenceCatalog.length} saved sequence(s) on this board? This can't be undone.`)) {
        apiPost("/api/sequences/clear").then(refreshSequenceList);
      }
    });
  }

  // ---------- sequences (Record tab list + Settings autostart picker) ----------
  let sequenceCatalog = []; // [{name,points,duration_ms}]

  async function refreshSequenceList() {
    sequenceCatalog = await apiGet("/api/sequences").catch(() => []);

    const body = $("seqBody");
    if (body) {
      body.innerHTML = sequenceCatalog.length
        ? sequenceCatalog
            .map(
              (s) => `
        <tr>
          <td>${s.name}</td>
          <td>${s.points}</td>
          <td>${(s.duration_ms / 1000).toFixed(1)}s</td>
          <td>
            <button class="btn btn-go" data-play="${s.name}">Play</button>
            <button class="btn btn-danger" data-delete="${s.name}">Delete</button>
          </td>
        </tr>`
            )
            .join("")
        : '<tr><td colspan="4">no sequences saved</td></tr>';
      body.querySelectorAll("[data-play]").forEach((btn) => {
        btn.addEventListener("click", () => apiPost("/api/sequence/play", { name: btn.dataset.play }));
      });
      body.querySelectorAll("[data-delete]").forEach((btn) => {
        btn.addEventListener("click", () => {
          if (confirm(`Delete sequence "${btn.dataset.delete}"?`)) {
            apiPost("/api/sequence/delete", { name: btn.dataset.delete }).then(refreshSequenceList);
          }
        });
      });
    }

    populateSequenceSelect($("asSequenceName"));
  }

  function populateSequenceSelect(selectEl) {
    if (!selectEl) return;
    const current = selectEl.value;
    selectEl.innerHTML = "";
    if (sequenceCatalog.length === 0) {
      const opt = document.createElement("option");
      opt.value = "";
      opt.textContent = "(no sequences saved)";
      selectEl.appendChild(opt);
      return;
    }
    sequenceCatalog.forEach((s) => {
      const opt = document.createElement("option");
      opt.value = s.name;
      opt.textContent = `${s.name} (${s.points} pts, ${(s.duration_ms / 1000).toFixed(1)}s)`;
      selectEl.appendChild(opt);
    });
    if ([...selectEl.options].some((o) => o.value === current)) selectEl.value = current;
  }

  // ---------- settings tab ----------
  async function loadSettings() {
    const s = await apiGet("/api/settings");

    $("setSsid").value = s.ap.ssid;
    $("setPassword").value = "";

    $("calMinUs").value = s.servo.min_us;
    $("calMaxUs").value = s.servo.max_us;
    $("calMinAngle").value = s.servo.min_angle;
    $("calMaxAngle").value = s.servo.max_angle;
    $("calCenterAngle").value = s.servo.center_angle;
    $("calInvert").checked = s.servo.invert;
    applyServoLimits(s.servo);

    if (s.relay) {
      $("relayActiveLow").checked = !!s.relay.active_low;
      $("relayPinLabel").textContent = `D7 (GPIO${s.relay.pin})`;
    }

    $("asEnabled").checked = s.autostart.enabled;
    $("asTarget").value = s.autostart.target;
    if (patternCatalog.length) {
      $("asPatternType").value = s.autostart.pattern.type;
      onAsPatternTypeChange();
      buildParamForm(
        $("asPatternParams"),
        catalogEntry(s.autostart.pattern.type).params.split(","),
        "ap",
        s.autostart.pattern
      );
    }
    await refreshSequenceList();
    $("asSequenceName").value = s.autostart.sequence_name || "";
    updateAsPatternVisibility();

    $("netMode").value = s.network.mode;
    $("netNodeId").value = s.network.node_id;
    updateNetVisibility();
  }

  function updateAsPatternVisibility() {
    const target = $("asTarget").value;
    const showPattern = target === "pattern";
    $("asPatternTypeField").style.display = showPattern ? "" : "none";
    $("asPatternParams").style.display = showPattern ? "" : "none";
    $("asSequenceField").style.display = target === "sequence" ? "" : "none";
  }

  // ---------- network (master/node) tab actions ----------
  let nodesPollTimer = null;

  function updateNetVisibility() {
    const mode = $("netMode").value;
    $("netNodeIdField").style.display = mode === "node" ? "" : "none";
    $("netMasterInfo").style.display = mode === "master" ? "" : "none";
  }

  // Shared by the Settings tab's known-nodes table and the Manual tab's
  // target picker (Master only) — one poll, two views of the same data.
  async function refreshKnownNodes() {
    if (!isMaster) return;
    const res = await apiGet("/api/network/nodes").catch(() => null);
    knownNodes = (res && res.nodes) || [];

    const body = $("netNodesBody");
    if (body) {
      body.innerHTML = knownNodes.length
        ? knownNodes
            .map(
              (n) =>
                `<tr><td>${n.id}</td><td>${n.angle.toFixed(1)}&deg;</td><td>${n.relay ? "on" : "off"}</td><td>${(n.age_ms / 1000).toFixed(1)}s ago</td></tr>`
            )
            .join("")
        : '<tr><td colspan="4">no nodes heard from yet</td></tr>';
    }

    renderTargetChecklist();
  }

  function renderTargetChecklist() {
    const container = $("targetNodesList");
    if (!container) return;

    const angleById = new Map(knownNodes.map((n) => [n.id, n.angle]));
    const ids = new Set([...knownNodes.map((n) => n.id), ...extraTargetNodes]);

    container.innerHTML = "";
    [...ids].sort((a, b) => a - b).forEach((id) => {
      const label = document.createElement("label");
      const input = document.createElement("input");
      input.type = "checkbox";
      input.disabled = targetAllMode;
      input.checked = selectedTargets.has(id);
      input.addEventListener("change", () => {
        if (input.checked) selectedTargets.add(id);
        else selectedTargets.delete(id);
      });
      const span = document.createElement("span");
      span.textContent = angleById.has(id) ? `Node ${id} (${angleById.get(id).toFixed(1)}°)` : `Node ${id}`;
      label.appendChild(input);
      label.appendChild(span);
      container.appendChild(label);
    });
    if (ids.size === 0) {
      container.innerHTML = '<span class="hint">no nodes heard from yet — add one by ID below</span>';
    }
  }

  function startNodesPolling() {
    if (nodesPollTimer) clearInterval(nodesPollTimer);
    refreshKnownNodes();
    nodesPollTimer = setInterval(refreshKnownNodes, 2000);
  }

  // ---------- target selector (Manual tab, Master only) ----------
  function initTargetControls() {
    $("targetAll").addEventListener("change", () => {
      targetAllMode = $("targetAll").checked;
      renderTargetChecklist();
    });

    $("targetAddNode").addEventListener("click", () => {
      const id = parseInt($("targetNodeId").value, 10);
      if (!Number.isInteger(id) || id < 1 || id > 250) return;
      extraTargetNodes.add(id);
      selectedTargets.add(id);
      targetAllMode = false;
      $("targetAll").checked = false;
      $("targetNodeId").value = "";
      renderTargetChecklist();
    });

    $("targetApply").addEventListener("click", async () => {
      await apiPost("/api/network/targets", {
        broadcast_all: targetAllMode,
        node_ids: [...selectedTargets],
      });
    });
  }

  // One settings fetch at boot: it carries the servo travel the jog fader and
  // pattern forms need, the relay's polarity/pin, and the Master-only bits.
  async function initFromSettings() {
    const settings = await apiGet("/api/settings").catch(() => null);
    if (!settings) return;

    applyServoLimits(settings.servo);
    if (settings.relay) {
      $("relayToggle").checked = !!settings.relay.on;
      $("relayPinLabel").textContent = `D7 (GPIO${settings.relay.pin})`;
    }

    isMaster = settings.network && settings.network.mode === "master";
    if (!isMaster) return;

    $("targetCard").style.display = "";
    const targets = await apiGet("/api/network/targets").catch(() => ({ broadcast_all: true, node_ids: [] }));
    targetAllMode = targets.broadcast_all !== false;
    selectedTargets = new Set(targets.node_ids || []);
    extraTargetNodes = new Set(targets.node_ids || []);
    $("targetAll").checked = targetAllMode;
    startNodesPolling();
  }

  function initSettingsControls() {
    $("asTarget").addEventListener("change", updateAsPatternVisibility);
    $("asPatternType").addEventListener("change", onAsPatternTypeChange);

    $("apSave").addEventListener("click", () => {
      const body = { ap: { ssid: $("setSsid").value } };
      const pw = $("setPassword").value;
      if (pw.length > 0) body.ap.password = pw;
      apiPost("/api/settings", body);
    });

    $("rebootBtn").addEventListener("click", () => {
      if (confirm("Reboot the servo controller now?")) apiPost("/api/reboot");
    });

    $("calSave").addEventListener("click", () => {
      const servo = {
        min_us: parseInt($("calMinUs").value, 10),
        max_us: parseInt($("calMaxUs").value, 10),
        min_angle: parseFloat($("calMinAngle").value),
        max_angle: parseFloat($("calMaxAngle").value),
        center_angle: parseFloat($("calCenterAngle").value),
        invert: $("calInvert").checked,
      };
      apiPost("/api/settings", { servo }).then(() => {
        // Re-range the jog fader and the pattern forms right away rather than
        // leaving them on the old travel until the next page load.
        applyServoLimits(servo);
        onPatternTypeChange();
        onAsPatternTypeChange();
      });
    });

    $("relaySave").addEventListener("click", () => {
      apiPost("/api/settings", { relay: { active_low: $("relayActiveLow").checked } });
    });

    $("asSave").addEventListener("click", () => {
      const entry = catalogEntry($("asPatternType").value);
      const pattern = readParamForm(entry.params.split(","), "ap", entry.type);
      apiPost("/api/settings", {
        autostart: {
          enabled: $("asEnabled").checked,
          target: $("asTarget").value,
          pattern,
          sequence_name: $("asSequenceName").value,
        },
      });
    });

    $("factoryReset").addEventListener("click", () => {
      if (confirm("Reset all settings to factory defaults?")) {
        apiPost("/api/settings/reset").then(loadSettings);
      }
    });

    $("netMode").addEventListener("change", () => {
      updateNetVisibility();
      refreshKnownNodes();
    });

    $("netSave").addEventListener("click", async () => {
      await apiPost("/api/settings", {
        network: {
          mode: $("netMode").value,
          node_id: parseInt($("netNodeId").value, 10),
        },
      });
      if (confirm("Network settings saved. Reboot now to apply?")) apiPost("/api/reboot");
    });
  }

  // ---------- boot ----------
  async function init() {
    initTabs();
    initJog();
    initRelay();
    initPatternControls();
    initRecordControls();
    initSettingsControls();
    initTargetControls();
    connectWs();
    // Settings first: the pattern forms built by loadPatterns() take their
    // angle bounds from the servo travel it reports.
    await initFromSettings();
    await loadPatterns();
    await refreshSequenceList();
  }

  document.addEventListener("DOMContentLoaded", init);
})();
