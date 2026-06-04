// SoftDips Manager Web frontend
//
//
// Browser support: in-place file/folder writing needs the File System Access
// API (Chrome, Chromium, Edge, etc)

let Module = null;

const hasFSAccess  = typeof window.showOpenFilePicker === "function";
const hasDirAccess = typeof window.showDirectoryPicker === "function";

// ── Application state ────────────────────────────────────────────────────────
const state = {
  mode: null,         // 'file' | 'folder'
  // single-file mode
  fileDoc: null, fileHandle: null, fileName: "settings.softdips",
  // folder mode
  dirHandle: null,
  games: [],          // GameEntry[]
  currentIndex: -1,
  dirty: false,       // current editor has edits not yet written to disk
};

// GameEntry: { name, dirHandle, hasSoftDips, softdipsHandle, romNames[],
//              hasRom, doc(Document|null), checked, status, auditProblem, audit }

const $ = (id) => document.getElementById(id);
const els = {
  status: $("status"), engineState: $("engineState"),
  openFile: $("openFile"), openFolder: $("openFolder"),
  save: $("save"), reset: $("reset"),
  toolsBtn: $("toolsBtn"), toolsMenu: $("toolsMenu"),
  cloneBtn: $("cloneBtn"), genAllBtn: $("genAllBtn"), genSelBtn: $("genSelBtn"), auditBtn: $("auditBtn"),
  settingsBtn: $("settingsBtn"), aboutBtn: $("aboutBtn"),
  selectAllRow: $("selectAllRow"), selectAll: $("selectAll"),
  gameList: $("gameList"), createFromRom: $("createFromRom"),
  gameLabel: $("gameLabel"), switchTable: $("switchTable"), switchBody: $("switchBody"),
  editorEmpty: $("editorEmpty"), log: $("log"),
  modalRoot: $("modalRoot"), fallbackInput: $("fallbackInput"),
};

// ── Small utilities ──────────────────────────────────────────────────────────
function setStatus(msg, kind = "") {
  els.status.textContent = msg;
  els.status.className = kind || "";
}
function log(msg) {
  const t = new Date().toLocaleTimeString([], { hour12: false });
  els.log.textContent += `[${t}] ${msg}\n`;
  els.log.scrollTop = els.log.scrollHeight;
}
function cleanLabel(s) {
  s = (s || "").trim();
  if (s.endsWith(":")) s = s.slice(0, -1).trim();
  return s.replace(/\s+/g, " ");
}

async function readFileBytes(handle) {
  return new Uint8Array(await (await handle.getFile()).arrayBuffer());
}
async function writeFileBytes(handle, bytes) {
  const w = await handle.createWritable();
  await w.write(bytes);
  await w.close();
}
async function verifyPerm(handle, mode = "readwrite") {
  const opts = { mode };
  if ((await handle.queryPermission(opts)) === "granted") return true;
  if ((await handle.requestPermission(opts)) === "granted") return true;
  return false;
}
function downloadBytes(bytes, name) {
  const url = URL.createObjectURL(new Blob([bytes], { type: "application/octet-stream" }));
  const a = document.createElement("a");
  a.href = url; a.download = name; a.click();
  URL.revokeObjectURL(url);
}

// ── IndexedDB (recent folders + last folder; stores FS handles) ──────────────
function idb() {
  return new Promise((res, rej) => {
    const r = indexedDB.open("softdips", 1);
    r.onupgradeneeded = () => r.result.createObjectStore("kv");
    r.onsuccess = () => res(r.result);
    r.onerror = () => rej(r.error);
  });
}
async function idbGet(key) {
  const db = await idb();
  return new Promise((res, rej) => {
    const t = db.transaction("kv", "readonly").objectStore("kv").get(key);
    t.onsuccess = () => res(t.result); t.onerror = () => rej(t.error);
  });
}
async function idbSet(key, val) {
  const db = await idb();
  return new Promise((res, rej) => {
    const t = db.transaction("kv", "readwrite").objectStore("kv").put(val, key);
    t.onsuccess = () => res(); t.onerror = () => rej(t.error);
  });
}
const getReopenLast = () => localStorage.getItem("reopenLast") !== "false";
const setReopenLast = (v) => localStorage.setItem("reopenLast", v ? "true" : "false");
const getRecentDirs = async () => (await idbGet("recentDirs")) || [];
async function addRecentDir(handle) {
  let r = (await getRecentDirs()).filter((x) => x.name !== handle.name);
  r.unshift({ name: handle.name, handle });
  await idbSet("recentDirs", r.slice(0, 20));
}
async function removeRecentDir(name) {
  await idbSet("recentDirs", (await getRecentDirs()).filter((x) => x.name !== name));
}
const setLastDir = (handle) => idbSet("lastDir", handle);
const getLastDir = () => idbGet("lastDir");

// ── Modal system ─────────────────────────────────────────────────────────────
function openModal(node) { els.modalRoot.innerHTML = ""; els.modalRoot.appendChild(node); els.modalRoot.hidden = false; }
function closeModal() { els.modalRoot.hidden = true; els.modalRoot.innerHTML = ""; }
function modalCard(title) {
  const card = document.createElement("div"); card.className = "modal";
  const h = document.createElement("h3"); h.textContent = title; card.appendChild(h);
  const body = document.createElement("div"); body.className = "body"; card.appendChild(body);
  const row = document.createElement("div"); row.className = "row"; card.appendChild(row);
  return { card, body, row };
}
function mkBtn(label, primary) {
  const b = document.createElement("button"); b.textContent = label;
  if (primary) b.style.borderColor = "var(--accent-2)";
  return b;
}
function para(text) { const p = document.createElement("p"); p.style.whiteSpace = "pre-wrap"; p.textContent = text; return p; }

function modalAlert(title, msg) {
  return new Promise((res) => {
    const { card, body, row } = modalCard(title);
    body.appendChild(para(msg));
    const ok = mkBtn("OK", true);
    ok.onclick = () => { closeModal(); res(); };
    row.appendChild(ok); openModal(card); ok.focus();
  });
}
function confirmModal(title, msg) {
  return new Promise((res) => {
    const { card, body, row } = modalCard(title);
    body.appendChild(para(msg));
    const no = mkBtn("No"), yes = mkBtn("Yes", true);
    no.onclick = () => { closeModal(); res(false); };
    yes.onclick = () => { closeModal(); res(true); };
    row.append(no, yes); openModal(card); yes.focus();
  });
}
function unsavedGuard() {
  return new Promise((res) => {
    const { card, body, row } = modalCard("Unsaved Changes");
    body.appendChild(para("You have unsaved changes to the current title. Save them?"));
    const c = mkBtn("Cancel"), d = mkBtn("Discard"), s = mkBtn("Save", true);
    c.onclick = () => { closeModal(); res("cancel"); };
    d.onclick = () => { closeModal(); res("discard"); };
    s.onclick = () => { closeModal(); res("save"); };
    row.append(c, d, s); openModal(card); s.focus();
  });
}

// ── Engine bootstrap ─────────────────────────────────────────────────────────
createSoftdipsModule().then(async (m) => {
  Module = m;
  els.engineState.textContent = "wasm: ready";

  if (!hasDirAccess) {
    els.openFolder.disabled = true;
    els.openFolder.title = "Folder workflow needs the File System Access API (use a Chromium browser).";
  }
  if (!hasFSAccess) {
    els.save.textContent = "Download…";
  }

  setStatus(hasFSAccess
    ? "Ready. Open a file, or a folder to manage a whole collection."
    : "Ready. This browser can't write files in place — edits download a copy you re-copy to the SD card. (Use a Chromium browser for in-place editing & the folder workflow.)");

  // Resume the last folder if we still hold permission (no prompt without a gesture).
  if (hasDirAccess && getReopenLast()) {
    try {
      const last = await getLastDir();
      if (last && (await last.queryPermission({ mode: "readwrite" })) === "granted") {
        await openFolderHandle(last);
      } else if (last) {
        setStatus(`Click “Open Folder” to resume “${last.name}”.`);
      }
    } catch (_) { /* ignore */ }
  }
}).catch((err) => {
  els.engineState.textContent = "wasm: failed";
  setStatus("Failed to load wasm engine: " + err, "error");
});

// ── Current-document helpers ─────────────────────────────────────────────────
function currentDoc() {
  if (state.mode === "file") return state.fileDoc;
  if (state.mode === "folder" && state.currentIndex >= 0) {
    const g = state.games[state.currentIndex];
    return g ? g.doc : null;
  }
  return null;
}
function currentSaveHandle() {
  if (state.mode === "file") return state.fileHandle;
  if (state.mode === "folder" && state.currentIndex >= 0) return state.games[state.currentIndex].softdipsHandle;
  return null;
}
function markDirty() { state.dirty = true; els.save.disabled = false; }

function clearFile() {
  if (state.fileDoc) { state.fileDoc.delete(); state.fileDoc = null; }
  state.fileHandle = null;
}
function clearFolder() {
  for (const g of state.games) if (g.doc) g.doc.delete();
  state.games = []; state.dirHandle = null; state.currentIndex = -1;
  els.gameList.innerHTML = "";
}
function replaceDoc(g, doc) { if (g.doc) g.doc.delete(); g.doc = doc; }

// ── Single-file open / load ──────────────────────────────────────────────────
els.openFile.addEventListener("click", async () => {
  if (!Module) return;
  try {
    if (hasFSAccess) {
      const [h] = await window.showOpenFilePicker({
        types: [{ description: "SoftDips", accept: { "application/octet-stream": [".softdips"] } }],
      });
      state.fileHandle = h; state.fileName = h.name;
      loadFileBytes(await readFileBytes(h));
    } else {
      els.fallbackInput.click();
    }
  } catch (err) {
    if (err && err.name === "AbortError") return;
    setStatus("Open failed: " + err, "error");
  }
});
els.fallbackInput.addEventListener("change", async (e) => {
  const file = e.target.files[0];
  if (!file) return;
  state.fileHandle = null; state.fileName = file.name;
  loadFileBytes(new Uint8Array(await file.arrayBuffer()));
  e.target.value = "";
});

function loadFileBytes(bytes) {
  clearFolder(); clearFile();
  const d = new Module.Document(bytes);
  if (!d.valid()) { d.delete(); setStatus("Not a valid .softdips file (need ≥32 bytes).", "error"); return; }
  state.mode = "file"; state.fileDoc = d; state.currentIndex = -1; state.dirty = false;
  els.selectAllRow.hidden = true; els.createFromRom.disabled = true; els.reset.disabled = true;
  renderEditor();
  els.save.disabled = true;
  setStatus(`Loaded ${state.fileName}` +
    (state.fileHandle ? " — edits save in place." : " — Save downloads a copy."), "ok");
  log("Loaded: " + d.gameName());
}

// ── Folder open / scan ───────────────────────────────────────────────────────
els.openFolder.addEventListener("click", openFolder);
async function openFolder() {
  if (!hasDirAccess) {
    modalAlert("Open Folder", "This browser lacks the File System Access API.\nUse a Chromium-based browser for the folder workflow.");
    return;
  }
  try {
    const h = await window.showDirectoryPicker({ mode: "readwrite" });
    await openFolderHandle(h);
  } catch (err) {
    if (err && err.name === "AbortError") return;
    setStatus("Open folder failed: " + err, "error");
  }
}
async function openFolderHandle(h) {
  if (!(await verifyPerm(h))) { setStatus("Permission denied for that folder.", "error"); return; }
  setStatus("Scanning…");
  clearFile(); clearFolder();
  state.mode = "folder"; state.dirHandle = h;
  state.games = await scanFolder(h);
  state.currentIndex = -1; state.dirty = false;
  els.selectAllRow.hidden = state.games.length === 0;
  renderGameList(); updateSelectAll();
  els.gameLabel.textContent = "Select a title from the list";
  log(`Loaded ${state.games.length} titles from “${h.name}”`);
  setStatus(`${state.games.length} titles`);
  try { await setLastDir(h); await addRecentDir(h); } catch (_) {}
  if (state.games.length) selectGame(0);
}
async function scanFolder(dirHandle) {
  const games = [];
  for await (const [name, handle] of dirHandle.entries()) {
    if (handle.kind !== "directory") continue;
    const g = { name, dirHandle: handle, hasSoftDips: false, softdipsHandle: null,
                romNames: [], hasRom: false, doc: null, checked: false,
                status: "none", auditProblem: false, audit: null };
    const fileNames = [];
    for await (const [fname, fh] of handle.entries()) {
      if (fh.kind !== "file") continue;
      fileNames.push(fname);
      if (fname === ".softdips") { g.hasSoftDips = true; g.softdipsHandle = fh; }
    }
    g.romNames = Module.rankProgramRoms(fileNames);
    g.hasRom = g.romNames.length > 0;
    if (g.hasSoftDips) {
      try {
        const d = new Module.Document(await readFileBytes(g.softdipsHandle));
        if (d.valid()) g.doc = d; else { d.delete(); g.hasSoftDips = false; }
      } catch (_) { g.hasSoftDips = false; }
    }
    g.status = g.hasSoftDips ? "has" : (g.hasRom ? "rom" : "none");
    games.push(g);
  }
  games.sort((a, b) => a.name.localeCompare(b.name));
  return games;
}

// ── Title list ───────────────────────────────────────────────────────────────
function glyphFor(g) {
  if (g.auditProblem) return "⚠";
  return g.status === "has" ? "✓" : g.status === "rom" ? "↻" : "○";
}
// A title is worth checking if it can be a clone target (has a .softdips) or a
// generate candidate (has a P-ROM but no .softdips yet). Only empty dirs (neither)
// are left uncheckable. The Tools each filter to the right subset of checked titles.
function isCheckable(g) { return (g.hasSoftDips && !!g.doc) || g.hasRom; }

function renderGameList() {
  els.gameList.innerHTML = "";
  state.games.forEach((g, i) => {
    const li = document.createElement("li");
    if (i === state.currentIndex) li.classList.add("selected");
    if (g.audit) li.title = g.audit;

    const cb = document.createElement("input");
    cb.type = "checkbox"; cb.checked = g.checked;
    cb.disabled = !isCheckable(g);
    cb.addEventListener("click", (e) => { e.stopPropagation(); g.checked = cb.checked; updateSelectAll(); });

    const glyph = document.createElement("span"); glyph.className = "glyph"; glyph.textContent = glyphFor(g);
    const name = document.createElement("span"); name.className = "gname"; name.textContent = g.name;

    li.append(cb, glyph, name);
    li.addEventListener("click", () => selectGame(i));
    els.gameList.appendChild(li);
  });
}
function updateSelectAll() {
  let selectable = 0, checked = 0;
  for (const g of state.games) {
    if (!isCheckable(g)) continue;
    selectable++; if (g.checked) checked++;
  }
  els.selectAll.indeterminate = checked > 0 && checked < selectable;
  els.selectAll.checked = selectable > 0 && checked === selectable;
}
els.selectAll.addEventListener("click", () => {
  const on = els.selectAll.checked;
  for (const g of state.games) g.checked = on && isCheckable(g);
  renderGameList(); updateSelectAll();
});

async function selectGame(i) {
  if (i !== state.currentIndex && state.currentIndex >= 0 && state.dirty) {
    const ans = await unsavedGuard();
    if (ans === "cancel") return;
    if (ans === "save") await doSave();
    if (ans === "discard") await revertCurrent();
  }
  state.currentIndex = i; state.dirty = false; els.save.disabled = true;
  const g = state.games[i];

  if (g.hasSoftDips && !g.doc) {
    try { const d = new Module.Document(await readFileBytes(g.softdipsHandle)); if (d.valid()) g.doc = d; else d.delete(); }
    catch (_) {}
  }
  renderGameList();

  if (!g.hasSoftDips || !g.doc) {
    els.gameLabel.textContent = g.name + " (no .softdips)";
    els.switchBody.innerHTML = ""; els.switchTable.hidden = true;
    els.editorEmpty.hidden = false;
    els.editorEmpty.textContent = g.hasRom
      ? "No .softdips yet — use “Create .softdips from P-ROM”."
      : "No .softdips and no program ROM in this folder.";
    els.reset.disabled = true;
    els.createFromRom.disabled = !g.hasRom;
    setStatus(g.name);
  } else {
    renderEditor();
    els.reset.disabled = !g.hasRom;
    els.createFromRom.disabled = true;
    setStatus(g.doc.gameName());
  }
}
async function revertCurrent() {
  if (state.mode !== "folder") return;
  const g = state.games[state.currentIndex];
  if (!g || !g.hasSoftDips) return;
  try {
    const d = new Module.Document(await readFileBytes(g.softdipsHandle));
    if (d.valid()) replaceDoc(g, d);
  } catch (_) {}
}

// ── Editor table ─────────────────────────────────────────────────────────────
function renderEditor() {
  const doc = currentDoc();
  els.switchBody.innerHTML = "";
  if (!doc) { els.switchTable.hidden = true; els.editorEmpty.hidden = false; return; }

  const model = JSON.parse(doc.json());
  els.gameLabel.textContent = model.gameName || "(unnamed)";
  const sws = model.switches;
  for (let i = 0; i < sws.length;) {
    const sw = sws[i];
    if (sw.kind === "time" && sw.timeField === 0) {
      let sec = null;
      if (i + 1 < sws.length) {
        const n = sws[i + 1];
        if (n.kind === "time" && n.timeField === 1 && n.metaByteIndex === sw.metaByteIndex) sec = n;
      }
      addTimeRow(doc, sw, sec);
      i += sec ? 2 : 1;
      continue;
    }
    addSelectRow(doc, sw);
    i++;
  }
  els.switchTable.hidden = false; els.editorEmpty.hidden = true;
}
function addSelectRow(doc, sw) {
  const tr = document.createElement("tr");
  const nameTd = document.createElement("td"); nameTd.className = "name";
  nameTd.textContent = cleanLabel(sw.name);
  if (sw.kind !== "list") {
    const k = document.createElement("span"); k.className = "kind"; k.textContent = sw.kind;
    nameTd.appendChild(k);
  }
  const valTd = document.createElement("td");
  const sel = document.createElement("select");
  sw.options.forEach((o, idx) => {
    const op = document.createElement("option");
    op.value = idx; op.textContent = cleanLabel(o) + (idx === sw.defaultIndex ? "  (default)" : "");
    sel.appendChild(op);
  });
  sel.value = String(sw.currentIndex);
  const mark = () => tr.classList.toggle("changed", parseInt(sel.value, 10) !== sw.defaultIndex);
  mark();
  sel.addEventListener("change", () => { doc.setCurrent(sw.index, parseInt(sel.value, 10)); mark(); markDirty(); });
  valTd.appendChild(sel);
  tr.append(nameTd, valTd); els.switchBody.appendChild(tr);
}
function addTimeRow(doc, minSw, secSw) {
  const tr = document.createElement("tr");
  const nameTd = document.createElement("td"); nameTd.className = "name";
  nameTd.textContent = cleanLabel(minSw.name).replace(/\s*\(MIN\)$/, "");
  const valTd = document.createElement("td");
  const cell = document.createElement("div"); cell.className = "time-cell";
  const addCombo = (sw, unit) => {
    const sel = document.createElement("select");
    sw.options.forEach((o, idx) => { const op = document.createElement("option"); op.value = idx; op.textContent = o; sel.appendChild(op); });
    sel.value = String(sw.currentIndex);
    sel.addEventListener("change", () => { doc.setCurrent(sw.index, parseInt(sel.value, 10)); markDirty(); });
    const u = document.createElement("span"); u.className = "unit"; u.textContent = unit;
    cell.append(sel, u);
  };
  addCombo(minSw, "min"); if (secSw) addCombo(secSw, "sec");
  valTd.appendChild(cell);
  tr.append(nameTd, valTd); els.switchBody.appendChild(tr);
}

// ── Save / Reset ─────────────────────────────────────────────────────────────
els.save.addEventListener("click", doSave);
async function doSave() {
  const doc = currentDoc(); if (!doc) return;
  const bytes = doc.toBytes();
  const handle = currentSaveHandle();
  try {
    if (handle) {
      await writeFileBytes(handle, bytes);
      const name = state.mode === "file" ? state.fileName : state.games[state.currentIndex].name;
      setStatus(`Saved ${name} in place.`, "ok"); log("Saved: " + name);
    } else {
      downloadBytes(bytes, state.fileName);
      setStatus(`Downloaded ${state.fileName}.`, "ok"); log("Downloaded: " + state.fileName);
    }
    state.dirty = false; els.save.disabled = true;
  } catch (err) {
    setStatus("Save failed: " + err, "error"); log("SAVE FAILED: " + err);
  }
}

els.reset.addEventListener("click", doReset);
async function doReset() {
  if (state.mode !== "folder") return;
  const g = state.games[state.currentIndex]; if (!g) return;
  const ex = await extractFromGameDir(g);
  if (!ex.found) { modalAlert("Reset to Defaults", "Couldn't read factory defaults from this title's program ROM.\n\n" + ex.diag); return; }
  if (!(await confirmModal("Reset to Defaults",
        `Reset "${g.doc ? g.doc.gameName() : g.name}" to its factory default soft DIP settings?\n\nThis discards the current settings for this title.`))) return;
  try {
    await writeFileBytes(g.softdipsHandle, ex.bytes);
    replaceDoc(g, new Module.Document(ex.bytes));
    renderEditor(); state.dirty = false; els.save.disabled = true;
    setStatus("Reset to defaults"); log("Reset to defaults: " + g.doc.gameName());
  } catch (err) {
    log("Reset FAILED to save: " + g.name + " — " + err);
  }
}

// Try each ranked P-ROM in a game's dir; return the first extractSoftdips hit.
async function extractFromGameDir(g) {
  for (const rn of g.romNames) {
    try {
      const fh = await g.dirHandle.getFileHandle(rn);
      const ex = Module.extractSoftdips(await readFileBytes(fh));
      if (ex.found) return ex;
    } catch (_) {}
  }
  return { found: false, diag: "No soft DIP settings found in this title's P-ROM (it may have none)." };
}

// ── Create .softdips from P-ROM ──────────────────────────────────────────────
els.createFromRom.addEventListener("click", createFromRom);
async function createFromRom() {
  if (state.mode !== "folder") return;
  const g = state.games[state.currentIndex]; if (!g || g.hasSoftDips) return;
  log(`Scanning ${g.name} for P-ROMs…`);
  const ex = await extractFromGameDir(g);
  if (!ex.found) {
    log("  No soft DIP settings found in this game's P-ROM (it may have none).");
    modalAlert("No Soft DIPs",
      "No soft DIP settings were found in this game's program ROM.\n\nThe game may simply have none (common for homebrew and early titles). See log for details.");
    return;
  }
  try {
    const fh = await g.dirHandle.getFileHandle(".softdips", { create: true });
    await writeFileBytes(fh, ex.bytes);
    g.softdipsHandle = fh; g.hasSoftDips = true; g.status = "has"; g.checked = false;
    replaceDoc(g, new Module.Document(ex.bytes));
    log("  ✓ Created .softdips");
    renderGameList(); selectGame(state.currentIndex);
  } catch (err) {
    log("  ✗ Failed to write .softdips: " + err);
  }
}

// ── Clone settings ───────────────────────────────────────────────────────────
els.cloneBtn.addEventListener("click", () => { closeToolsMenu(); doClone(); });
async function doClone() {
  if (state.mode !== "folder") { modalAlert("Clone Settings", "Open a folder of titles first."); return; }
  const src = currentDoc();
  if (!src) { modalAlert("Clone Settings", "Open a title with settings to use as the source first."); return; }
  const targets = state.games.filter((g) => g.checked && g.hasSoftDips && g.doc);
  if (!targets.length) { modalAlert("Clone Settings", "Check one or more titles on the left to clone settings onto."); return; }

  const model = JSON.parse(src.json());
  const settings = model.switches.map((s) => {
    const value = s.options[s.currentIndex] ?? "";
    return { name: s.name, value, label: `${cleanLabel(s.name)}  =  ${cleanLabel(value || "?")}` };
  });
  const chosen = await cloneSettingsDialog(model.gameName, targets, settings);
  if (chosen === null) return;
  if (!chosen.length) { modalAlert("Clone Settings", "No settings were selected to copy."); return; }
  await executeClone(chosen, targets);
}
function cloneSettingsDialog(sourceName, targets, settings) {
  return new Promise((res) => {
    const { card, body, row } = modalCard("Clone Settings");
    const intro = document.createElement("p");
    intro.append(document.createTextNode("Copy settings from "));
    const b1 = document.createElement("b"); b1.textContent = sourceName; intro.appendChild(b1);
    intro.append(document.createTextNode(" to these "));
    const b2 = document.createElement("b"); b2.textContent = targets.length; intro.appendChild(b2);
    intro.append(document.createTextNode(" title(s):"));
    body.appendChild(intro);

    const tgt = document.createElement("div"); tgt.className = "targets";
    tgt.textContent = targets.map((t) => t.doc.gameName()).join(", ");
    body.appendChild(tgt);

    const lbl = document.createElement("p"); lbl.textContent = "Settings to copy:"; body.appendChild(lbl);
    const selAllL = document.createElement("label"); selAllL.className = "checkrow";
    const selAll = document.createElement("input"); selAll.type = "checkbox";
    selAllL.append(selAll, document.createTextNode(" Select all settings")); body.appendChild(selAllL);

    const list = document.createElement("div"); list.className = "scrolllist";
    const boxes = [];
    settings.forEach((s) => {
      const l = document.createElement("label");
      const cb = document.createElement("input"); cb.type = "checkbox";
      l.append(cb, document.createTextNode(" " + s.label));
      list.appendChild(l); boxes.push(cb);
    });
    body.appendChild(list);

    const upd = () => {
      const c = boxes.filter((b) => b.checked).length;
      selAll.indeterminate = c > 0 && c < boxes.length;
      selAll.checked = boxes.length > 0 && c === boxes.length;
    };
    selAll.onclick = () => { boxes.forEach((b) => (b.checked = selAll.checked)); upd(); };
    boxes.forEach((b) => (b.onchange = upd));

    const cancel = mkBtn("Cancel"), apply = mkBtn("Apply…", true);
    cancel.onclick = () => { closeModal(); res(null); };
    apply.onclick = () => { closeModal(); res(settings.filter((_, i) => boxes[i].checked)); };
    row.append(cancel, apply); openModal(card);
  });
}
function resolveDialog(gameName, concept, desired, candidates) {
  return new Promise((res) => {
    const { card, body, row } = modalCard("Confirm Setting");
    const p = document.createElement("p");
    const b0 = document.createElement("b"); b0.textContent = gameName;
    const b1 = document.createElement("b"); b1.textContent = concept;
    const b2 = document.createElement("b"); b2.textContent = desired;
    p.append(b0, document.createElement("br"), document.createTextNode("Set "), b1, document.createTextNode(" to "), b2, document.createTextNode(" — choose how to apply it:"));
    body.appendChild(p);
    const sel = document.createElement("select");
    candidates.forEach((c, i) => { const o = document.createElement("option"); o.value = i; o.textContent = `${c.switchName}  →  ${c.optionName}`; sel.appendChild(o); });
    body.appendChild(sel);
    const repL = document.createElement("label"); repL.className = "checkrow";
    const rep = document.createElement("input"); rep.type = "checkbox";
    repL.append(rep, document.createTextNode(" Apply this decision to all remaining titles for this setting"));
    body.appendChild(repL);
    const skip = mkBtn("Skip"), apply = mkBtn("Apply", true);
    const done = (isSkip) => {
      const c = candidates[parseInt(sel.value, 10)];
      closeModal();
      res({ skip: isSkip, repeat: rep.checked, switchName: c ? c.switchName : "", optionIndex: c ? c.optionIndex : -1, optionName: c ? c.optionName : "" });
    };
    skip.onclick = () => done(true); apply.onclick = () => done(false);
    row.append(skip, apply); openModal(card);
  });
}
async function executeClone(toApply, targets) {
  // Preview tally.
  let nConfident = 0, nAmbiguous = 0, nNotFound = 0;
  for (const s of toApply)
    for (const g of targets) {
      const m = JSON.parse(g.doc.matchSetting(s.name, s.value));
      if (m.kind === "confident") nConfident++;
      else if (m.kind === "ambiguous") nAmbiguous++;
      else nNotFound++;
    }
  const lines = [];
  if (nConfident) lines.push(`${nConfident} will apply automatically`);
  if (nAmbiguous) lines.push(`${nAmbiguous} need confirming (you'll be asked)`);
  if (nNotFound) lines.push(`${nNotFound} have no matching switch (skipped)`);
  lines.push("", "See log for full details.");
  if (!(await confirmModal("Clone Apply Preview",
        `Apply ${toApply.length} setting(s) to ${targets.length} title(s)?\n\n${lines.join("\n")}`))) {
    log("─── Clone Apply CANCELLED by user ───"); return;
  }

  log("─── Clone Apply ───");
  const modified = new Set();
  for (const s of toApply) {
    log(`• ${s.name} = ${s.value}`);
    let repeatMode = "ask"; // 'ask' | 'auto' | 'skipall'
    for (const g of targets) {
      const gn = g.doc.gameName();
      const m = JSON.parse(g.doc.matchSetting(s.name, s.value));
      let useSwitch = null, useOpt = -1, useName = "";
      if (m.kind === "notfound") { log(`   ⚠ ${gn}: no matching switch — skipped`); continue; }
      else if (m.kind === "confident") { useSwitch = m.candidates[0].switchName; useOpt = m.candidates[0].optionIndex; useName = m.candidates[0].optionName; }
      else {
        if (repeatMode === "skipall") { log(`   ⚠ ${gn}: skipped (repeat)`); continue; }
        else if (repeatMode === "auto") { useSwitch = m.candidates[0].switchName; useOpt = m.candidates[0].optionIndex; useName = m.candidates[0].optionName; }
        else {
          const ch = await resolveDialog(gn, s.name, s.value, m.candidates);
          if (ch.repeat) repeatMode = ch.skip ? "skipall" : "auto";
          if (ch.skip) { log(`   ⚠ ${gn}: skipped by user`); continue; }
          useSwitch = ch.switchName; useOpt = ch.optionIndex; useName = ch.optionName;
        }
      }
      if (g.doc.setByName(useSwitch, useOpt)) {
        modified.add(g);
        log(`   ✓ ${gn}: ${useSwitch} = ${useName}`);
      }
    }
  }

  let saved = 0, failed = 0;
  for (const g of modified) {
    try { await writeFileBytes(g.softdipsHandle, g.doc.toBytes()); saved++; }
    catch (_) { failed++; log("   ✗ save failed: " + g.name); }
  }
  log(`─── Done: ${saved} saved, ${nNotFound} not found${failed ? `, ${failed} save error(s)` : ""} ───`);
  if (state.currentIndex >= 0) renderEditor();
}

// ── Generate .softdips ───────────────────────────────────────────────────────
els.genAllBtn.addEventListener("click", () => { closeToolsMenu(); doGenerate(state.games.map((_, i) => i), "title(s)"); });
els.genSelBtn.addEventListener("click", () => {
  closeToolsMenu();
  const sel = state.games.map((g, i) => (g.checked ? i : -1)).filter((i) => i >= 0);
  if (!sel.length) { modalAlert("Generate", "Check one or more titles on the left first."); return; }
  doGenerate(sel, "selected title(s)");
});
async function doGenerate(indices, noun) {
  if (state.mode !== "folder" || !state.games.length) { modalAlert("Generate", "Open a folder first."); return; }
  const cands = indices.filter((i) => !state.games[i].hasSoftDips && state.games[i].hasRom);
  if (!cands.length) {
    modalAlert("Generate", `None of the ${noun} need a .softdips generated.\n\n(Existing files are left untouched. Use Audit to refresh stale ones.)`);
    return;
  }
  if (!(await confirmModal("Generate .softdips",
        `Create .softdips for ${cands.length} ${noun} that don't have one yet?\n\nExisting files are left untouched. Titles with no soft DIPs (demos) are skipped.`))) return;

  log("─── Generate .softdips ───");
  let created = 0, skipped = 0, failed = 0;
  for (const i of cands) {
    const g = state.games[i];
    const ex = await extractFromGameDir(g);
    if (!ex.found) { skipped++; continue; }
    try {
      const fh = await g.dirHandle.getFileHandle(".softdips", { create: true });
      await writeFileBytes(fh, ex.bytes);
      g.softdipsHandle = fh; g.hasSoftDips = true; g.status = "has";
      replaceDoc(g, new Module.Document(ex.bytes));
      created++; log("  ✓ " + g.name);
    } catch (_) { failed++; log("  ✗ " + g.name + ": write failed"); }
  }
  renderGameList(); if (state.currentIndex >= 0) selectGame(state.currentIndex);
  log(`─── Created ${created}, skipped ${skipped} (no soft DIPs)${failed ? `, ${failed} failed` : ""} ───`);
  modalAlert("Generate Complete", `Created ${created} .softdips file(s).\n${skipped} title(s) had no soft DIPs (skipped).`);
}

// ── Audit ────────────────────────────────────────────────────────────────────
els.auditBtn.addEventListener("click", () => { closeToolsMenu(); doAudit(); });
async function auditGame(g) {
  const ex = await extractFromGameDir(g);
  if (!g.hasSoftDips) {
    if (!ex.found) return { skip: true };
    return { skip: false, status: "nv", text: "no .softdips", diffs: [], tip: "Audit: no .softdips" };
  }
  if (!ex.found) {
    const t = g.hasRom ? "ROM present but no softdips table" : "no program ROM to verify against";
    return { skip: false, status: "nv", text: t, diffs: [], tip: "Audit: " + t };
  }
  const cmp = Module.compareStructure(g.doc.toBytes(), ex.bytes);
  if (cmp.ok) return { skip: false, status: "ok", text: "OK (matches P-ROM)", diffs: [], tip: "Audit: matches P-ROM table" };
  const diffs = []; for (let k = 0; k < cmp.diffs.length; k++) diffs.push(cmp.diffs[k]);
  return { skip: false, status: "problem", text: "MISMATCH (stale or wrong ROM)", diffs, tip: ["Audit: MISMATCH (stale or wrong ROM)", ...diffs].join("\n") };
}
async function doAudit() {
  if (state.mode !== "folder" || !state.games.length) { modalAlert("Audit", "Open a folder first."); return; }
  log("─── Audit .softdips vs P-ROM ───");
  let okCount = 0, problem = 0, notVerified = 0;
  const regenerable = [];
  for (let i = 0; i < state.games.length; i++) {
    const g = state.games[i];
    const r = await auditGame(g);
    if (r.skip) { g.audit = null; continue; }
    g.audit = r.tip;
    if (r.status === "ok") { okCount++; g.auditProblem = false; log("  ✓ " + g.name + " — OK"); }
    else if (r.status === "problem") {
      problem++; g.auditProblem = true; log("  ✗ " + g.name + " — " + r.text);
      r.diffs.forEach((d) => log("       - " + d));
      if (g.hasRom) regenerable.push(i);
    } else { notVerified++; g.auditProblem = false; log("  · " + g.name + " — " + r.text); }
  }
  renderGameList();
  log(`─── ${okCount} OK, ${problem} problem(s), ${notVerified} not verified ───`);
  const summary = `${okCount} OK\n${problem} problem(s)\n${notVerified} not verified`;
  if (!regenerable.length) { modalAlert("Audit Complete", summary + "\n\nSee log for details."); return; }
  if (!(await confirmModal("Audit Complete",
        summary + `\n\n${regenerable.length} problem title(s) can be regenerated from their P-ROM. Regenerate now?\n\n(This overwrites those .softdips files with fresh data from the P-ROM.)`))) return;

  let rebuilt = 0;
  for (const i of regenerable) {
    const g = state.games[i];
    const ex = await extractFromGameDir(g);
    if (!ex.found) { log("  ✗ " + g.name + ": " + ex.diag); continue; }
    try {
      await writeFileBytes(g.softdipsHandle, ex.bytes);
      replaceDoc(g, new Module.Document(ex.bytes));
      g.auditProblem = false; g.audit = "Audit: regenerated from P-ROM"; rebuilt++;
      log("  ✓ Regenerated " + g.name);
    } catch (_) { log("  ✗ " + g.name + ": write failed"); }
  }
  renderGameList(); if (state.currentIndex >= 0) selectGame(state.currentIndex);
  log(`─── Regenerated ${rebuilt} .softdips file(s) ───`);
  modalAlert("Regenerated", `Regenerated ${rebuilt} .softdips file(s) from their P-ROMs.`);
}

// ── Settings & About ─────────────────────────────────────────────────────────
els.settingsBtn.addEventListener("click", settingsDialog);
async function settingsDialog() {
  const { card, body, row } = modalCard("Settings");
  const g = document.createElement("label"); g.className = "checkrow";
  const cb = document.createElement("input"); cb.type = "checkbox"; cb.checked = getReopenLast();
  cb.onchange = () => setReopenLast(cb.checked);
  g.append(cb, document.createTextNode(" Reopen last working folder on startup")); body.appendChild(g);

  if (!hasDirAccess) {
    const note = para("Saved folders need the File System Access API (Chromium).");
    note.style.color = "var(--muted)"; note.style.marginTop = "12px"; body.appendChild(note);
  } else {
    const h = para("Saved folders (click to select):"); h.style.marginTop = "12px"; body.appendChild(h);
    const ul = document.createElement("ul"); ul.className = "recent"; body.appendChild(ul);
    let selected = -1;
    const recents = await getRecentDirs();
    recents.forEach((r, i) => {
      const li = document.createElement("li"); li.textContent = r.name;
      li.onclick = () => { selected = i; [...ul.children].forEach((c) => c.classList.remove("selected")); li.classList.add("selected"); };
      ul.appendChild(li);
    });
    if (!recents.length) { const li = document.createElement("li"); li.textContent = "(none yet)"; li.style.color = "var(--muted)"; ul.appendChild(li); }

    const add = mkBtn("Add…"), remove = mkBtn("Remove"), open = mkBtn("Open", true);
    add.onclick = () => { closeModal(); openFolder(); };
    remove.onclick = async () => { if (selected < 0) return; await removeRecentDir(recents[selected].name); closeModal(); settingsDialog(); };
    open.onclick = async () => {
      if (selected < 0) return;
      const r = recents[selected]; closeModal();
      if (await verifyPerm(r.handle)) await openFolderHandle(r.handle);
      else modalAlert("Open", "Permission denied or folder no longer accessible.");
    };
    row.append(add, remove, open);
  }
  const close = mkBtn("Close"); close.onclick = closeModal; row.appendChild(close);
  openModal(card);
}

els.aboutBtn.addEventListener("click", () => {
  const { card, body, row } = modalCard("About SoftDips Manager");
  const d = document.createElement("div"); d.className = "center";
  d.innerHTML =
    "SoftDips Manager (Web)<br>For the BackBit NeoGeo MVS Platinum Cartridge<br><br>" +
    'by morb — <a href="https://meson.ninja/" target="_blank" rel="noopener">meson.ninja</a><br>' +
    '<a href="https://github.com/m0rb/softdips-manager" target="_blank" rel="noopener">github.com/m0rb/softdips-manager</a><br><br>';
  body.appendChild(d);
  const ok = mkBtn("OK", true); ok.onclick = closeModal; row.appendChild(ok);
  openModal(card);
});

// ── Tools dropdown ───────────────────────────────────────────────────────────
function closeToolsMenu() { els.toolsMenu.hidden = true; }
els.toolsBtn.addEventListener("click", (e) => { e.stopPropagation(); els.toolsMenu.hidden = !els.toolsMenu.hidden; });
document.addEventListener("click", (e) => {
  if (!els.toolsMenu.hidden && !els.toolsMenu.contains(e.target) && e.target !== els.toolsBtn) closeToolsMenu();
});

// Warn before leaving with unsaved edits.
window.addEventListener("beforeunload", (e) => {
  if (state.dirty) { e.preventDefault(); e.returnValue = ""; }
});
