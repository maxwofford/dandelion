// gloam shared logic — no DOM deps, used by both browser and CLI
//
// in browser: loaded via <script src>, all functions become globals.
//             index.html's own globals (nodes, edges, etc) are used directly.
//
// in bun: require()'d, returns an object with init() to set up module-level state.

// --- math ---
function mat4Ortho(l, r, b, t) {
  return new Float32Array([
    2/(r-l),0,0,0, 0,2/(t-b),0,0, 0,0,-1,0, -(r+l)/(r-l),-(t+b)/(t-b),0,1
  ]);
}
function hslToRgb(h, s, l) {
  const a = s * Math.min(l, 1-l);
  const f = n => { const k = (n + h/30) % 12; return l - a * Math.max(-1, Math.min(k-3, 9-k, 1)); };
  return [f(0), f(8), f(4)];
}

// --- color from file extension ---
function extToColor(ext) {
  if (!ext) return [0.6, 0.6, 0.6];
  let h = 0;
  for (let i = 0; i < ext.length; i++) h = ((h << 5) - h + ext.charCodeAt(i)) | 0;
  return hslToRgb(Math.abs(h) % 360, 0.65, 0.55);
}

// --- git log parser ---
function parseGitLog(text) {
  const lines = text.split('\n');
  const commits = [];
  let i = 0;
  while (i < lines.length) {
    if (!lines[i].trim()) { i++; continue; }
    const parts = lines[i].split('|');
    if (parts.length < 4) { i++; continue; }
    const commit = {
      timestamp: parseInt(parts[0]),
      author: parts[1],
      hash: parts[2],
      message: parts.slice(3).join('|'),
      actions: []
    };
    i++;
    while (i < lines.length && lines[i].trim()) {
      const line = lines[i];
      const action = line[0];
      const path = line.substring(1).trim();
      if (path && 'AMD'.includes(action)) {
        commit.actions.push({ type: action, path });
      }
      i++;
    }
    if (commit.actions.length > 0) commits.push(commit);
  }
  return commits;
}

// --- tree building from commit range ---
function buildTreeFromCommits(commits, upTo) {
  const tree = { name: '', children: new Map(), isDir: true };
  for (let c = 0; c < upTo; c++) {
    for (const act of commits[c].actions) {
      const segs = act.path.split('/');
      let node = tree;
      for (let j = 0; j < segs.length; j++) {
        const name = segs[j];
        const isFile = j === segs.length - 1;
        if (!node.children.has(name)) {
          node.children.set(name, {
            name, children: new Map(), isDir: !isFile,
            ext: isFile ? name.split('.').pop().toLowerCase() : '',
            deleted: false
          });
        }
        const child = node.children.get(name);
        if (act.type === 'D' && isFile) child.deleted = true;
        if (act.type === 'A' && isFile) child.deleted = false;
        node = child;
      }
    }
  }
  return tree;
}

// --- radial tree layout ---
function layoutTree(root) {
  const nodes = [];
  const edges = [];

  const FILE_AREA = Math.PI * 4 * 4;
  const DIR_PADDING = 1.5;

  function leafCount(n) {
    if (n._lc !== undefined) return n._lc;
    const kids = [...n.children.values()].filter(c => !c.deleted);
    n._lc = kids.length === 0 ? 1 : kids.reduce((s, c) => s + leafCount(c), 0);
    return n._lc;
  }

  function treeArea(n) {
    if (n._area !== undefined) return n._area;
    const kids = [...n.children.values()].filter(c => !c.deleted);
    const files = kids.filter(c => !c.isDir);
    const dirs = kids.filter(c => c.isDir);
    const fileArea = FILE_AREA * files.length;
    n._area = fileArea + dirs.reduce((s, c) => s + treeArea(c), 0);
    n._fileArea = fileArea;
    return n._area;
  }
  function treeRadius(n) { return Math.max(15, Math.sqrt(treeArea(n)) * DIR_PADDING); }
  function treeParentRadius(n) { return Math.max(1, Math.sqrt(n._fileArea || 0)) * DIR_PADDING; }

  nodes.push({
    x: 0, y: 0, r: 8, col: [1.0, 0.7, 0.3, 1], birth: 0,
    isDir: true, parentIdx: -1, vx: 0, vy: 0, path: ''
  });

  function lay(node, depth, aStart, aEnd, px, py, pIdx, prefix) {
    const kids = [...node.children.values()].filter(c => !c.deleted);
    const dirs = kids.filter(c => c.isDir);
    const files = kids.filter(c => !c.isDir);

    for (const f of files) {
      const col = extToColor(f.ext);
      const path = prefix ? prefix + '/' + f.name : f.name;
      nodes.push({
        x: px, y: py, r: 4, col: [...col, 1], birth: depth * 0.04,
        isDir: false, dirIdx: pIdx, path
      });
    }

    if (dirs.length === 0) return;
    const dirTotal = dirs.reduce((s, c) => s + leafCount(c), 0);
    let angle = aStart;

    for (const c of dirs) {
      const lc = leafCount(c);
      const span = (aEnd - aStart) * lc / dirTotal;
      const mid = angle + span / 2;
      const edgeLen = treeRadius(c) + treeParentRadius(node);
      const x = px + Math.cos(mid) * edgeLen;
      const y = py + Math.sin(mid) * edgeLen;

      const idx = nodes.length;
      const dirPath = prefix ? prefix + '/' + c.name : c.name;
      nodes.push({
        x, y, r: 6, col: [0.8, 0.6, 0.3, 1], birth: depth * 0.04,
        isDir: true, parentIdx: pIdx, vx: 0, vy: 0, lc, path: dirPath
      });
      edges.push([pIdx, idx]);

      lay(c, depth + 1, angle, angle + span, x, y, idx, dirPath);
      angle += span;
    }
  }

  lay(root, 1, 0, Math.PI * 2, 0, 0, 0, '');
  return { nodes, edges };
}

// --- test scene ---
function makeTestTree() {
  const root = { name:'', children:new Map(), isDir:true, deleted:false, ext:'' };
  function addDir(parent, name, fileNames) {
    const dir = { name, children:new Map(), isDir:true, deleted:false, ext:'' };
    for (const fn of fileNames) {
      dir.children.set(fn, {
        name:fn, children:new Map(), isDir:false,
        ext:fn.split('.').pop().toLowerCase(), deleted:false
      });
    }
    parent.children.set(name, dir);
    return dir;
  }
  const src = addDir(root, 'src', [
    'main.cpp','app.cpp','app.h','utils.cpp','utils.h',
    'config.cpp','config.h','render.cpp','render.h',
    'shader.cpp','shader.h','window.cpp'
  ]);
  addDir(src, 'core', ['engine.cpp','engine.h','math.cpp','math.h','types.h','logger.cpp']);
  addDir(root, 'data', ['config.json','theme.css','icon.svg']);
  addDir(root, 'test', ['test_main.cpp','test_utils.cpp','test_render.cpp','test_config.cpp']);
  addDir(root, 'docs', ['README.md','INSTALL.md','CHANGELOG.md']);
  for (const fn of ['Makefile','README.md','.gitignore','LICENSE']) {
    root.children.set(fn, {
      name:fn, children:new Map(), isDir:false,
      ext:fn.split('.').pop().toLowerCase(), deleted:false
    });
  }
  return root;
}

// --- deterministic hash ---
function pathHash(str) {
  let h = 0;
  for (let i = 0; i < str.length; i++) h = ((h << 5) - h + str.charCodeAt(i)) | 0;
  return h;
}

// --- exports for CLI usage via bun ---
if (typeof module !== 'undefined' && module.exports) {
  module.exports = {
    mat4Ortho, hslToRgb, extToColor, parseGitLog, buildTreeFromCommits,
    layoutTree, makeTestTree, pathHash,
  };
}
