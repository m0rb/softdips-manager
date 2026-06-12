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
  history: { undo: [], redo: [] },  // per-title undo stacks of {switchIndex, prev, next}
};

// GameEntry: { name, dirHandle, hasSoftDips, softdipsHandle, romNames[],
//              hasRom, doc(Document|null), checked, status, auditProblem, audit }

const $ = (id) => document.getElementById(id);
const els = {
  status: $("status"), engineState: $("engineState"),
  openFile: $("openFile"), openFolder: $("openFolder"),
  save: $("save"), reset: $("reset"),
  toolsBtn: $("toolsBtn"), toolsMenu: $("toolsMenu"),
  cloneBtn: $("cloneBtn"), setEverywhereBtn: $("setEverywhereBtn"),
  bulkExportBtn: $("bulkExportBtn"), bulkImportBtn: $("bulkImportBtn"),
  backupBtn: $("backupBtn"), restoreBtn: $("restoreBtn"),
  genAllBtn: $("genAllBtn"), genSelBtn: $("genSelBtn"), auditBtn: $("auditBtn"),
  exportBtn: $("exportBtn"), importBtn: $("importBtn"), shareBtn: $("shareBtn"),
  changedOnly: $("changedOnly"),
  shareBanner: $("shareBanner"), shareBannerText: $("shareBannerText"),
  shareApply: $("shareApply"), shareDismiss: $("shareDismiss"),
  settingsBtn: $("settingsBtn"), aboutBtn: $("aboutBtn"),
  selectAllRow: $("selectAllRow"), selectAll: $("selectAll"), titleFilter: $("titleFilter"),
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

  checkSharedLink();
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
function markDirty() { state.dirty = true; els.save.disabled = false; updateTitleDirty(); }
function clearDirty() { state.dirty = false; els.save.disabled = true; clearHistory(); updateTitleDirty(); }
function clearHistory() { state.history.undo.length = 0; state.history.redo.length = 0; }
// Mark the current title in the list as having unsaved edits (only the open
// title can be dirty — switching titles is guarded).
function updateTitleDirty() {
  const lis = els.gameList.children;
  for (let i = 0; i < lis.length; i++) lis[i].classList.toggle("dirty", i === state.currentIndex && state.dirty);
}
// Record an undoable switch edit (the editor's setCurrent already happened).
function pushUndo(switchIndex, prev, next) {
  if (prev === next) return;
  state.history.undo.push({ switchIndex, prev, next });
  state.history.redo.length = 0;
  state.dirty = true; els.save.disabled = false; updateTitleDirty();
}
function doUndo() {
  const doc = currentDoc(); if (!doc || !state.history.undo.length) return;
  const e = state.history.undo.pop();
  doc.setCurrent(e.switchIndex, e.prev);
  state.history.redo.push(e);
  refreshDirtyFromHistory(); renderEditor();
}
function doRedo() {
  const doc = currentDoc(); if (!doc || !state.history.redo.length) return;
  const e = state.history.redo.pop();
  doc.setCurrent(e.switchIndex, e.next);
  state.history.undo.push(e);
  refreshDirtyFromHistory(); renderEditor();
}
// After undo/redo, the editor matches disk iff the undo stack is empty.
function refreshDirtyFromHistory() {
  state.dirty = state.history.undo.length > 0;
  els.save.disabled = !state.dirty;
  updateTitleDirty();
}

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
  state.mode = "file"; state.fileDoc = d; state.currentIndex = -1; state.dirty = false; clearHistory();
  els.selectAllRow.hidden = true; els.titleFilter.hidden = true; els.createFromRom.disabled = true; els.reset.disabled = true;
  els.exportBtn.disabled = false; els.importBtn.disabled = false; els.shareBtn.disabled = false;
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
  state.currentIndex = -1; state.dirty = false; clearHistory();
  els.selectAllRow.hidden = state.games.length === 0;
  els.titleFilter.hidden = state.games.length === 0; els.titleFilter.value = "";
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
  applyTitleFilter(); updateTitleDirty();
}
els.titleFilter.addEventListener("input", applyTitleFilter);
function applyTitleFilter() {
  const q = (els.titleFilter.value || "").trim().toLowerCase();
  const lis = els.gameList.children;
  for (let i = 0; i < lis.length && i < state.games.length; i++) {
    const g = state.games[i];
    const hay = (g.name + " " + (g.doc ? g.doc.gameName() : "")).toLowerCase();
    lis[i].style.display = (!q || hay.includes(q)) ? "" : "none";
  }
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
  state.currentIndex = i; state.dirty = false; els.save.disabled = true; clearHistory();
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
    els.exportBtn.disabled = true; els.importBtn.disabled = true; els.shareBtn.disabled = true;
    setStatus(g.name);
  } else {
    renderEditor();
    els.reset.disabled = !g.hasRom;
    els.createFromRom.disabled = true;
    els.exportBtn.disabled = false; els.importBtn.disabled = false; els.shareBtn.disabled = false;
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
  applyChangedFilter();
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
  let prev = sw.currentIndex;
  const mark = () => tr.classList.toggle("changed", parseInt(sel.value, 10) !== sw.defaultIndex);
  mark();
  sel.addEventListener("change", () => {
    const v = parseInt(sel.value, 10);
    doc.setCurrent(sw.index, v); pushUndo(sw.index, prev, v); prev = v; mark();
  });
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
    let prev = sw.currentIndex;
    sel.addEventListener("change", () => {
      const v = parseInt(sel.value, 10);
      doc.setCurrent(sw.index, v); pushUndo(sw.index, prev, v); prev = v;
    });
    const u = document.createElement("span"); u.className = "unit"; u.textContent = unit;
    cell.append(sel, u);
  };
  addCombo(minSw, "min"); if (secSw) addCombo(secSw, "sec");
  tr.classList.toggle("changed",
    minSw.currentIndex !== minSw.defaultIndex || (secSw && secSw.currentIndex !== secSw.defaultIndex));
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
    clearDirty();
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
    renderEditor(); clearDirty();
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
async function executeClone(toApply, targets, label = "Clone Apply") {
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
  if (!(await confirmModal(`${label} Preview`,
        `Apply ${toApply.length} setting(s) to ${targets.length} title(s)?\n\n${lines.join("\n")}`))) {
    log(`─── ${label} CANCELLED by user ───`); return;
  }

  log(`─── ${label} ───`);
  const modified = await resolveApplyToDocs(toApply, targets);

  let saved = 0, failed = 0;
  for (const g of modified) {
    try { await writeFileBytes(g.softdipsHandle, g.doc.toBytes()); saved++; }
    catch (_) { failed++; log("   ✗ save failed: " + g.name); }
  }
  log(`─── Done: ${saved} saved, ${nNotFound} not found${failed ? `, ${failed} save error(s)` : ""} ───`);
  if (state.currentIndex >= 0) renderEditor();
}

// Resolve a {name, value} list against each target's doc and apply it (mutating
// the docs, NOT saving). Confident matches apply automatically; ambiguous ones
// prompt via resolveDialog (with a per-setting "apply to all remaining" repeat).
// Returns the set of targets whose doc actually changed. Shared by Clone, Import,
// and bulk "set a setting everywhere".
async function resolveApplyToDocs(toApply, targets) {
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
  return modified;
}

// ── Settings profiles: export / import / bulk "set everywhere" ───────────────
// A profile is a portable, name/value snapshot of a title's selections:
//   { app:"softdips-manager", v:1, gameName, settings:[{name,value},…] }
// Name/value (not raw indices) survives structural differences and applies
// through the same matchSetting machinery as Clone. This format is shared with
// the Qt app so profiles interoperate between the two.

function profileFromDoc(doc) {
  const model = JSON.parse(doc.json());
  return {
    app: "softdips-manager",
    v: 1,
    gameName: model.gameName,
    settings: model.switches.map((s) => ({ name: s.name, value: s.options[s.currentIndex] ?? "" })),
  };
}
// Parse + validate a profile JSON string. Returns { ok, profile?, error? }.
function parseProfile(text) {
  let obj;
  try { obj = JSON.parse(text); } catch (e) { return { ok: false, error: "Not valid JSON." }; }
  if (!obj || !Array.isArray(obj.settings))
    return { ok: false, error: "Not a SoftDips settings profile (missing 'settings')." };
  const settings = obj.settings
    .filter((s) => s && typeof s.name === "string" && typeof s.value === "string")
    .map((s) => ({ name: s.name, value: s.value }));
  if (!settings.length) return { ok: false, error: "Profile has no usable settings." };
  return { ok: true, profile: { gameName: obj.gameName || "", settings } };
}
function downloadText(text, name) {
  const url = URL.createObjectURL(new Blob([text], { type: "application/json" }));
  const a = document.createElement("a");
  a.href = url; a.download = name; a.click();
  URL.revokeObjectURL(url);
}
function safeFileName(s) { return (s || "settings").trim().replace(/[^A-Za-z0-9 ._-]+/g, "_") || "settings"; }

// Export the current title's settings as a downloadable profile JSON.
els.exportBtn.addEventListener("click", exportSettings);
function exportSettings() {
  const doc = currentDoc();
  if (!doc) { modalAlert("Export Settings", "Open a title first."); return; }
  const profile = profileFromDoc(doc);
  const name = safeFileName(profile.gameName) + ".softdips.json";
  downloadText(JSON.stringify(profile, null, 2), name);
  setStatus(`Exported settings → ${name}`, "ok");
  log(`Exported settings: ${name} (${profile.settings.length} setting(s))`);
}

const sameName = (a, b) => (a || "").trim().toUpperCase() === (b || "").trim().toUpperCase();
// Pick a JSON file (FS Access picker, else fallback input). Returns text or null.
async function pickJsonText() {
  if (hasFSAccess) {
    const [h] = await window.showOpenFilePicker({
      types: [{ description: "JSON", accept: { "application/json": [".json"] } }],
    });
    return await (await h.getFile()).text();
  }
  return await pickTextFileFallback();
}

// Import a profile from a file and apply it to the title it's for.
els.importBtn.addEventListener("click", importSettings);
async function importSettings() {
  if (!currentDoc()) { modalAlert("Import Settings", "Open a title to import settings into first."); return; }
  let text;
  try { text = await pickJsonText(); if (text == null) return; }
  catch (err) { if (err && err.name === "AbortError") return; setStatus("Import failed: " + err, "error"); return; }
  const parsed = parseProfile(text);
  if (!parsed.ok) { modalAlert("Import Settings", "Couldn't import: " + parsed.error); return; }
  await applyProfile(parsed.profile);
}

// Apply a profile to the title it's for (detected by gameName), offering to
// switch to that title first. Shared by Import and the share-link banner.
async function applyProfile(profile) {
  const settings = profile.settings;
  const wantName = (profile.gameName || "").trim();
  if (state.mode === "folder") {
    const match = state.games.find((g) => g.doc && sameName(g.doc.gameName(), wantName));
    if (match) {
      if (state.games[state.currentIndex] !== match) {
        if (!(await confirmModal("Apply Settings", `These settings are for "${match.doc.gameName()}". Switch to that title and apply?`))) return;
        await selectGame(state.games.indexOf(match));
        if (state.games[state.currentIndex] !== match) return; // switch cancelled
      }
      await executeClone(settings, [match], "Apply");
    } else {
      const cur = state.games[state.currentIndex];
      const curName = cur.doc ? cur.doc.gameName() : cur.name;
      if (!(await confirmModal("Apply Settings",
            `These settings are for "${wantName || "(unknown)"}", which isn't in this folder.\n\nApply to the current title "${curName}" anyway?`))) return;
      await executeClone(settings, [cur], "Apply");
    }
  } else {
    const cur = state.fileDoc;
    if (wantName && !sameName(cur.gameName(), wantName) &&
        !(await confirmModal("Apply Settings",
          `These settings are for "${wantName}" but the open file is "${cur.gameName()}".\n\nApply anyway?`))) return;
    log(`─── Apply into ${cur.gameName()} ───`);
    const modified = await resolveApplyToDocs(settings, [{ doc: cur }]);
    if (modified.size) { markDirty(); renderEditor(); setStatus("Applied — review and Save.", "ok"); }
    else setStatus("No settings matched this title.");
  }
}

// ── Share: a base64 code / link of the current title's settings ──────────────
const b64url = (s) => btoa(unescape(encodeURIComponent(s))).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
function unb64url(s) { s = s.replace(/-/g, "+").replace(/_/g, "/"); while (s.length % 4) s += "="; return decodeURIComponent(escape(atob(s))); }
// A share input may be a bare base64 code, a share link (…#s=CODE), or raw JSON.
function decodeShare(s) {
  s = (s || "").trim();
  const h = s.indexOf("#s="); if (h >= 0) s = s.slice(h + 3).trim();
  if (s.startsWith("{")) return s;
  try { return unb64url(s); } catch (_) { return s; }
}

els.shareBtn.addEventListener("click", shareDialog);
function shareDialog() {
  const doc = currentDoc(); if (!doc) return;
  const profile = profileFromDoc(doc);
  const code = b64url(JSON.stringify(profile));
  const link = location.origin + location.pathname + "#s=" + code;
  const { card, body, row } = modalCard("Share Settings");
  body.appendChild(para(`A share code for "${profile.gameName || "this title"}" (${profile.settings.length} setting(s)). Copy it, or paste a code / link / JSON here and Apply:`));
  const ta = document.createElement("textarea");
  ta.value = code; ta.rows = 4; ta.style.width = "100%"; ta.style.resize = "vertical"; ta.style.marginTop = "6px";
  ta.onfocus = () => ta.select();
  body.appendChild(ta);
  const copy = (text, what) => navigator.clipboard.writeText(text).then(() => setStatus(what + " copied.", "ok"), () => setStatus("Copy failed (needs a secure context).", "error"));
  const copyBtn = mkBtn("Copy"), linkBtn = mkBtn("Copy Link"), applyBtn = mkBtn("Apply", true), close = mkBtn("Close");
  copyBtn.onclick = () => copy(ta.value, "Share code");
  linkBtn.onclick = () => copy(link, "Share link");
  applyBtn.onclick = async () => {
    const parsed = parseProfile(decodeShare(ta.value));
    if (!parsed.ok) { setStatus("Not a valid share code or profile.", "error"); return; }
    closeModal(); await applyProfile(parsed.profile);
  };
  close.onclick = closeModal;
  row.append(copyBtn, linkBtn, applyBtn, close); openModal(card); ta.focus();
}

// A #s=<base64url-profile> in the URL offers a one-time apply via a banner.
let pendingShared = null;
function checkSharedLink() {
  if (!location.hash.startsWith("#s=")) return;
  try {
    const parsed = parseProfile(unb64url(location.hash.slice(3)));
    if (parsed.ok) {
      pendingShared = parsed.profile;
      els.shareBannerText.textContent =
        `Shared settings for "${parsed.profile.gameName || "a title"}" (${parsed.profile.settings.length} setting(s)). Open the title, then Apply.`;
      els.shareBanner.hidden = false;
    }
  } catch (_) {}
  history.replaceState(null, "", location.pathname + location.search);
}
els.shareApply.addEventListener("click", async () => {
  if (!pendingShared) return;
  if (!currentDoc()) { modalAlert("Apply Shared Settings", "Open the title first, then click Apply."); return; }
  const p = pendingShared;
  await applyProfile(p);
  els.shareBanner.hidden = true; pendingShared = null;
});
els.shareDismiss.addEventListener("click", () => { els.shareBanner.hidden = true; pendingShared = null; });

// ── "Changed only" filter: hide rows at their ROM default ────────────────────
els.changedOnly.addEventListener("change", applyChangedFilter);
function applyChangedFilter() {
  const on = els.changedOnly.checked;
  for (const tr of els.switchBody.children)
    tr.style.display = (!on || tr.classList.contains("changed")) ? "" : "none";
}

// ── Backup / restore: a .zip of every .softdips in the open folder ───────────
// Minimal stored (uncompressed) ZIP — files are tiny, so no deflate needed.
const crcTable = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1); t[n] = c >>> 0; }
  return t;
})();
function crc32(bytes) { let c = 0xFFFFFFFF; for (let i = 0; i < bytes.length; i++) c = crcTable[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; }

function zipStore(files) {
  const enc = new TextEncoder();
  const chunks = []; let offset = 0;
  const push = (u8) => { chunks.push(u8); offset += u8.length; };
  const u16 = (v) => { const a = new Uint8Array(2); new DataView(a.buffer).setUint16(0, v, true); return a; };
  const u32 = (v) => { const a = new Uint8Array(4); new DataView(a.buffer).setUint32(0, v >>> 0, true); return a; };
  const central = [];
  for (const f of files) {
    const name = enc.encode(f.name), crc = crc32(f.data), localOff = offset;
    push(u32(0x04034b50)); push(u16(20)); push(u16(0)); push(u16(0)); push(u16(0)); push(u16(0));
    push(u32(crc)); push(u32(f.data.length)); push(u32(f.data.length));
    push(u16(name.length)); push(u16(0)); push(name); push(f.data);
    central.push({ name, crc, size: f.data.length, localOff });
  }
  const cdStart = offset;
  for (const c of central) {
    push(u32(0x02014b50)); push(u16(20)); push(u16(20)); push(u16(0)); push(u16(0)); push(u16(0)); push(u16(0));
    push(u32(c.crc)); push(u32(c.size)); push(u32(c.size));
    push(u16(c.name.length)); push(u16(0)); push(u16(0)); push(u16(0)); push(u16(0));
    push(u32(0)); push(u32(c.localOff)); push(c.name);
  }
  const cdSize = offset - cdStart;
  push(u32(0x06054b50)); push(u16(0)); push(u16(0)); push(u16(central.length)); push(u16(central.length));
  push(u32(cdSize)); push(u32(cdStart)); push(u16(0));
  const out = new Uint8Array(offset); let p = 0;
  for (const ch of chunks) { out.set(ch, p); p += ch.length; }
  return out;
}
function zipRead(bytes) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const dec = new TextDecoder();
  const out = []; let p = 0;
  while (p + 4 <= bytes.length && dv.getUint32(p, true) === 0x04034b50) {
    const method = dv.getUint16(p + 8, true);
    const size = dv.getUint32(p + 18, true);
    const nameLen = dv.getUint16(p + 26, true), extraLen = dv.getUint16(p + 28, true);
    const name = dec.decode(bytes.subarray(p + 30, p + 30 + nameLen));
    const dataStart = p + 30 + nameLen + extraLen;
    if (method === 0) out.push({ name, data: bytes.slice(dataStart, dataStart + size) });
    p = dataStart + size;
  }
  return out;
}
function timestamp() {
  const d = new Date(), z = (n) => String(n).padStart(2, "0");
  return `${d.getFullYear()}${z(d.getMonth() + 1)}${z(d.getDate())}-${z(d.getHours())}${z(d.getMinutes())}${z(d.getSeconds())}`;
}
async function pickZipBytes() {
  if (hasFSAccess) {
    const [h] = await window.showOpenFilePicker({ types: [{ description: "Zip", accept: { "application/zip": [".zip"] } }] });
    return new Uint8Array(await (await h.getFile()).arrayBuffer());
  }
  return new Promise((res) => {
    const inp = document.createElement("input"); inp.type = "file"; inp.accept = ".zip";
    inp.onchange = async () => { const f = inp.files[0]; res(f ? new Uint8Array(await f.arrayBuffer()) : null); };
    inp.click();
  });
}

els.backupBtn.addEventListener("click", () => { closeToolsMenu(); downloadBackup(); });
async function downloadBackup() {
  if (state.mode !== "folder") { modalAlert("Backup", "Open a folder first."); return; }
  const files = [];
  for (const g of state.games) {
    if (!g.hasSoftDips || !g.softdipsHandle) continue;
    try { files.push({ name: g.name + "/.softdips", data: await readFileBytes(g.softdipsHandle) }); } catch (_) {}
  }
  if (!files.length) { modalAlert("Backup", "No .softdips files to back up."); return; }
  const name = safeFileName(state.dirHandle ? state.dirHandle.name : "softdips") + "-backup-" + timestamp() + ".zip";
  downloadBytes(zipStore(files), name);
  setStatus(`Backed up ${files.length} file(s) → ${name}`, "ok");
  log(`Backup: ${files.length} .softdips → ${name}`);
}

els.restoreBtn.addEventListener("click", () => { closeToolsMenu(); restoreBackup(); });
async function restoreBackup() {
  if (state.mode !== "folder") { modalAlert("Restore", "Open the folder to restore into first."); return; }
  let bytes;
  try { bytes = await pickZipBytes(); if (!bytes) return; }
  catch (err) { if (err && err.name === "AbortError") return; setStatus("Restore failed: " + err, "error"); return; }
  let entries;
  try { entries = zipRead(bytes); } catch (e) { modalAlert("Restore", "Couldn't read the backup: " + e); return; }

  const jobs = []; let unmatched = 0;
  for (const e of entries) {
    const m = e.name.match(/^(.*)\/[^/]*\.softdips$/) || e.name.match(/^(.*)\/\.softdips$/);
    const g = m ? state.games.find((x) => x.name === m[1]) : null;
    if (g) jobs.push({ game: g, data: e.data }); else unmatched++;
  }
  if (!jobs.length) { modalAlert("Restore", "No files in the backup match this folder."); return; }
  if (!(await confirmModal("Restore Backup",
        `Overwrite .softdips for ${jobs.length} title(s) from the backup?${unmatched ? `\n${unmatched} file(s) had no match (skipped).` : ""}\n\nThis replaces their current settings.`))) return;

  log("─── Restore Backup ───");
  let restored = 0, failed = 0;
  for (const j of jobs) {
    try {
      let fh = j.game.softdipsHandle || await j.game.dirHandle.getFileHandle(".softdips", { create: true });
      await writeFileBytes(fh, j.data);
      j.game.softdipsHandle = fh; j.game.hasSoftDips = true; j.game.status = "has";
      replaceDoc(j.game, new Module.Document(j.data));
      restored++; log("  ✓ " + j.game.name);
    } catch (e) { failed++; log("  ✗ " + j.game.name + ": " + e); }
  }
  renderGameList(); if (state.currentIndex >= 0) selectGame(state.currentIndex);
  log(`─── Restore done: ${restored} restored${failed ? `, ${failed} error(s)` : ""} ───`);
  setStatus(`Restored ${restored} title(s).`, "ok");
}

// ── Bulk export / import: a whole-collection settings file ───────────────────
els.bulkExportBtn.addEventListener("click", () => { closeToolsMenu(); bulkExport(); });
function bulkExport() {
  if (state.mode !== "folder") { modalAlert("Export All Settings", "Open a folder first."); return; }
  const titles = state.games.filter((g) => g.hasSoftDips && g.doc).map((g) => {
    const p = profileFromDoc(g.doc);
    return { gameName: p.gameName, dir: g.name, settings: p.settings };
  });
  if (!titles.length) { modalAlert("Export All Settings", "No titles with settings to export."); return; }
  const name = safeFileName(state.dirHandle ? state.dirHandle.name : "collection") + ".softdips-collection.json";
  downloadText(JSON.stringify({ app: "softdips-manager", v: 1, type: "collection", titles }, null, 2), name);
  setStatus(`Exported ${titles.length} title(s) → ${name}`, "ok");
  log(`Bulk exported ${titles.length} title(s).`);
}

els.bulkImportBtn.addEventListener("click", () => { closeToolsMenu(); bulkImport(); });
async function bulkImport() {
  if (state.mode !== "folder") { modalAlert("Import Settings Collection", "Open a folder first."); return; }
  let text;
  try { text = await pickJsonText(); if (text == null) return; }
  catch (err) { if (err && err.name === "AbortError") return; setStatus("Import failed: " + err, "error"); return; }

  let obj; try { obj = JSON.parse(text); } catch (e) { modalAlert("Import Settings Collection", "Not valid JSON."); return; }
  if (!obj || !Array.isArray(obj.titles)) { modalAlert("Import Settings Collection", "Not a collection file (missing 'titles')."); return; }

  // Match each entry to a loaded title by folder name, then by game name.
  const jobs = []; let unmatched = 0;
  for (const t of obj.titles) {
    const settings = (t.settings || []).filter((s) => s && typeof s.name === "string" && typeof s.value === "string")
      .map((s) => ({ name: s.name, value: s.value }));
    if (!settings.length) continue;
    let g = state.games.find((x) => x.doc && x.hasSoftDips && x.name === t.dir);
    if (!g) g = state.games.find((x) => x.doc && sameName(x.doc.gameName(), t.gameName));
    if (g) jobs.push({ game: g, settings }); else unmatched++;
  }
  if (!jobs.length) { modalAlert("Import Settings Collection", "None of the titles in this file match the open folder."); return; }
  if (!(await confirmModal("Import Settings Collection",
        `Apply settings to ${jobs.length} matched title(s)?${unmatched ? `\n${unmatched} title(s) had no match (skipped).` : ""}\n\nAmbiguous matches will prompt.`))) return;

  log("─── Bulk Import ───");
  const modified = new Set();
  for (const j of jobs) for (const g of await resolveApplyToDocs(j.settings, [j.game])) modified.add(g);
  let saved = 0, failed = 0;
  for (const g of modified) {
    try { await writeFileBytes(g.softdipsHandle, g.doc.toBytes()); saved++; }
    catch (_) { failed++; log("   ✗ save failed: " + g.name); }
  }
  log(`─── Bulk Import done: ${saved} saved${failed ? `, ${failed} error(s)` : ""} ───`);
  if (state.currentIndex >= 0) renderEditor();
  setStatus(`Bulk import: ${saved} title(s) updated.`, "ok");
}

// ── Bulk: set one setting across many titles (no source game needed) ─────────
els.setEverywhereBtn.addEventListener("click", () => { closeToolsMenu(); setSettingEverywhere(); });
async function setSettingEverywhere() {
  if (state.mode !== "folder" || !state.games.length) { modalAlert("Set a Setting Across Titles", "Open a folder of titles first."); return; }
  const games = state.games.filter((g) => g.hasSoftDips && g.doc);
  if (!games.length) { modalAlert("Set a Setting Across Titles", "No titles with settings are loaded."); return; }

  // Build a catalog: cleaned setting name → set of option values seen across titles.
  const catalog = new Map();
  for (const g of games) {
    for (const sw of JSON.parse(g.doc.json()).switches) {
      const name = cleanLabel(sw.name);
      if (!catalog.has(name)) catalog.set(name, new Set());
      const vals = catalog.get(name);
      for (const o of sw.options) vals.add(cleanLabel(o));
    }
  }
  const choice = await setEverywhereDialog(catalog, games.length);
  if (!choice) return;
  // matchSetting normalizes names/values, so this resolves across naming variants.
  await executeClone([{ name: choice.name, value: choice.value }], games, "Bulk Set");
}
function setEverywhereDialog(catalog, titleCount) {
  return new Promise((res) => {
    const { card, body, row } = modalCard("Set a Setting Across Titles");
    body.appendChild(para(`Pick a setting and value to apply to all ${titleCount} loaded title(s) that have it. Titles without that setting are skipped; ambiguous matches will ask.`));

    const names = [...catalog.keys()].sort((a, b) => a.localeCompare(b));
    const nameSel = document.createElement("select");
    names.forEach((n) => { const o = document.createElement("option"); o.value = n; o.textContent = n; nameSel.appendChild(o); });
    body.appendChild(labelRow("Setting:", nameSel));

    const valSel = document.createElement("select");
    body.appendChild(labelRow("Value:", valSel));
    const fillValues = () => {
      valSel.innerHTML = "";
      [...catalog.get(nameSel.value)].sort((a, b) => a.localeCompare(b))
        .forEach((v) => { const o = document.createElement("option"); o.value = v; o.textContent = v; valSel.appendChild(o); });
    };
    nameSel.onchange = fillValues; fillValues();

    const cancel = mkBtn("Cancel"), apply = mkBtn("Apply…", true);
    cancel.onclick = () => { closeModal(); res(null); };
    apply.onclick = () => { closeModal(); res({ name: nameSel.value, value: valSel.value }); };
    row.append(cancel, apply); openModal(card);
  });
}
// Small helpers shared by the dialogs above.
function labelRow(text, control) {
  const wrap = document.createElement("div"); wrap.style.margin = "8px 0";
  const l = document.createElement("div"); l.textContent = text; l.style.marginBottom = "2px"; l.style.color = "var(--muted)"; l.style.fontSize = "12px";
  wrap.append(l, control); return wrap;
}
// Fallback JSON file pick for browsers without showOpenFilePicker.
function pickTextFileFallback() {
  return new Promise((res) => {
    const inp = document.createElement("input");
    inp.type = "file"; inp.accept = ".json,application/json";
    inp.onchange = async () => { const f = inp.files[0]; res(f ? await f.text() : null); };
    inp.click();
  });
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
    '<a href="https://github.com/m0rb/softdips-manager" target="_blank" rel="noopener">github.com/m0rb/softdips-manager</a>' +
    '<br><br>Thanks to evie, HornHeaDD, NeoGeo81, lithy, and<br>' +
    'the rest of The BackBit Forum™ community</center>';
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

// Undo / redo (Ctrl/Cmd+Z, Ctrl/Cmd+Y or Ctrl/Cmd+Shift+Z) when not in a modal.
document.addEventListener("keydown", (e) => {
  if (!els.modalRoot.hidden) return;
  const mod = e.ctrlKey || e.metaKey; if (!mod) return;
  const k = e.key.toLowerCase();
  if (k === "z" && !e.shiftKey) { e.preventDefault(); doUndo(); }
  else if (k === "y" || (k === "z" && e.shiftKey)) { e.preventDefault(); doRedo(); }
});

// Warn before leaving with unsaved edits.
window.addEventListener("beforeunload", (e) => {
  if (state.dirty) { e.preventDefault(); e.returnValue = ""; }
});
