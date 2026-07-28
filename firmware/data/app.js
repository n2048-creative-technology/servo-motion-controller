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
  }

  function updateAsPatternVisibility() {
    const show = $("asTarget").value === "pattern";
    $("asPatternTypeField").style.display = show ? "" : "none";
    $("asPatternParams").style.display = show ? "" : "none";
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
  }

  // ---------- boot ----------
  async function init() {
    initTabs();
    initJog();
    initPatternControls();
    initRecordControls();
    initSettingsControls();
    connectWs();
    await loadPatterns();
    await refreshSequenceInfo();
  }

  document.addEventListener("DOMContentLoaded", init);
})();
