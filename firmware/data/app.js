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
  };

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
    $("angleLabel").textContent = `${s.angle.toFixed(1)}°`;

    const recActive = s.recording && s.recording.active;
    $("recDot").classList.toggle("dot-on", !!recActive);
    $("recDot").classList.toggle("dot-off", !recActive);
    $("recLabel").textContent = recActive ? "recording" : "idle";
    $("recPoints").textContent = `${s.recording ? s.recording.points : 0} pts`;

    if (recActive) {
      recTrace.push({ t: s.uptime_ms, angle: s.angle });
      if (recTrace.length > 400) recTrace.shift();
      drawTrace();
    } else if (recTrace.length && s.recording && s.recording.points === 0) {
      recTrace = [];
      drawTrace();
    }

    if (s.sequence) {
      $("seqInfo").textContent = s.sequence.present
        ? `${s.sequence.points} points, ${(s.sequence.duration_ms / 1000).toFixed(1)}s`
        : "no sequence saved";
    }

    // Keep the jog slider in sync when the servo is being driven by
    // something other than this browser (pattern/sequence/another client).
    if (document.activeElement !== $("jogSlider")) {
      $("jogSlider").value = s.angle;
      $("jogValue").textContent = s.angle.toFixed(1);
    }
  }

  function drawTrace() {
    const canvas = $("recCanvas");
    const ctx = canvas.getContext("2d");
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    if (recTrace.length < 2) return;

    const tMin = recTrace[0].t;
    const tMax = recTrace[recTrace.length - 1].t;
    const tSpan = Math.max(1, tMax - tMin);

    ctx.strokeStyle = "#4da3ff";
    ctx.lineWidth = 2;
    ctx.beginPath();
    recTrace.forEach((p, i) => {
      const x = ((p.t - tMin) / tSpan) * canvas.width;
      const y = canvas.height - (p.angle / 270) * canvas.height;
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
    $("recSave").addEventListener("click", () => apiPost("/api/record/save").then(refreshSequenceInfo));
    $("recDiscard").addEventListener("click", () => {
      recTrace = [];
      drawTrace();
      apiPost("/api/record/discard").then(refreshSequenceInfo);
    });
    $("seqPlay").addEventListener("click", () => apiPost("/api/sequence/play"));
    $("seqStop").addEventListener("click", () => apiPost("/api/sequence/stop"));
  }

  async function refreshSequenceInfo() {
    const seq = await apiGet("/api/sequence");
    $("seqInfo").textContent = seq.present
      ? `${seq.points} points, ${(seq.duration_ms / 1000).toFixed(1)}s`
      : "no sequence saved";
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
    updateAsPatternVisibility();

    $("netMode").value = s.network.mode;
    $("netNodeId").value = s.network.node_id;
    updateNetVisibility();
  }

  function updateAsPatternVisibility() {
    const show = $("asTarget").value === "pattern";
    $("asPatternTypeField").style.display = show ? "" : "none";
    $("asPatternParams").style.display = show ? "" : "none";
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
            .map((n) => `<tr><td>${n.id}</td><td>${n.angle.toFixed(1)}&deg;</td><td>${(n.age_ms / 1000).toFixed(1)}s ago</td></tr>`)
            .join("")
        : '<tr><td colspan="3">no nodes heard from yet</td></tr>';
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

  async function initMasterUi() {
    const settings = await apiGet("/api/settings");
    isMaster = settings.network.mode === "master";
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
      apiPost("/api/settings", {
        servo: {
          min_us: parseInt($("calMinUs").value, 10),
          max_us: parseInt($("calMaxUs").value, 10),
          min_angle: parseFloat($("calMinAngle").value),
          max_angle: parseFloat($("calMaxAngle").value),
          center_angle: parseFloat($("calCenterAngle").value),
        },
      });
    });

    $("asSave").addEventListener("click", () => {
      const entry = catalogEntry($("asPatternType").value);
      const pattern = readParamForm(entry.params.split(","), "ap", entry.type);
      apiPost("/api/settings", {
        autostart: {
          enabled: $("asEnabled").checked,
          target: $("asTarget").value,
          pattern,
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
      refreshNodesTable();
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
    initPatternControls();
    initRecordControls();
    initSettingsControls();
    initTargetControls();
    connectWs();
    await loadPatterns();
    await refreshSequenceInfo();
    await initMasterUi();
  }

  document.addEventListener("DOMContentLoaded", init);
})();
