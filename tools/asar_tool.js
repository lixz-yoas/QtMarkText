/*
 * asar_tool.js —— 严格按 asar 磁盘布局实现的列表 / 抽取工具
 *
 * asar 布局（对照 electron/asar 的 src/pickle.js + src/disk.js）：
 *   [0..4)   uint32 = 4                     // headerSizePickle 的 payload 长度
 *   [4..8)   uint32 = headerPickleBuf.length // header pickle 整体长度
 *   [8.. )   headerPickleBuf：
 *              [ uint32 payloadSize ][ uint32 jsonLen ][ json 字节 ][ 对齐 padding ]
 *   内容区起点 = 8 + headerPickleBuf.length
 *   每个文件项 { size, offset:"字符串" }，offset 相对内容区起点
 *
 * 用法：
 *   node asar_tool.js list        <asar> [前缀]
 *   node asar_tool.js get         <asar> <内部路径> [输出文件]
 *   node asar_tool.js verify      <asar> <内部路径>   // 打印 size 与尾部 200 字节
 *   node asar_tool.js extractall  <asar> <内部前缀> <输出目录>
 */
'use strict';
const fs = require('fs');
const path = require('path');

function readHeader(asarPath) {
  const fd = fs.openSync(asarPath, 'r');
  const b8 = Buffer.alloc(8);
  fs.readSync(fd, b8, 0, 8, 0);
  const headerPickleLen = b8.readUInt32LE(4);
  const headerBuf = Buffer.alloc(headerPickleLen);
  fs.readSync(fd, headerBuf, 0, headerPickleLen, 8);
  const jsonLen = headerBuf.readUInt32LE(4);
  const json = headerBuf.slice(8, 8 + jsonLen).toString('utf8');
  const contentStart = 8 + headerPickleLen;
  return { fd, header: JSON.parse(json), contentStart };
}

function lookup(header, innerPath) {
  // 内部路径形如 /preload/index.js
  const parts = innerPath.replace(/^\//, '').split('/');
  let node = { files: header.files };
  for (const p of parts) {
    if (!node.files || !node.files[p]) return null;
    node = node.files[p];
  }
  return node;
}

const [, , cmd, asarPath, a1, a2] = process.argv;
if (!cmd || !asarPath) {
  console.error('用法: node asar_tool.js list|get|verify|extractall <asar> ...');
  process.exit(2);
}
const { fd, header, contentStart } = readHeader(asarPath);
console.error('contentStart =', contentStart);

/** 收集某前缀下的全部文件项 */
function nodeAt(header, prefix) {
  const parts = prefix.replace(/^\//, '').split('/').filter(Boolean);
  let node = { files: header.files };
  for (const p of parts) {
    if (!node.files || !node.files[p]) return null;
    node = node.files[p];
  }
  return node;
}

function collect(node, p, out) {
  for (const name of Object.keys(node.files || {})) {
    const child = node.files[name];
    const full = p + '/' + name;
    if (child.files) collect(child, full, out);
    else out.push([full, child]);
  }
  return out;
}

if (cmd === 'list') {
  const prefix = a1 || '';
  const out = [];
  (function walk(node, p) {
    for (const name of Object.keys(node.files || {})) {
      const child = node.files[name];
      const full = p + '/' + name;
      if (child.files) walk(child, full);
      else out.push([full, child.size, child.offset, child.unpacked ? 'UNPACKED' : '']);
    }
  })(header, '');
  for (const [f, s, o, u] of out) {
    if (!prefix || f.startsWith(prefix)) console.log(f + '\t' + s + '\t' + o + '\t' + u);
  }
} else if (cmd === 'get' || cmd === 'verify') {
  const node = lookup(header, a1);
  if (!node || node.files) {
    console.error('找不到文件项: ' + a1);
    process.exit(1);
  }
  if (node.unpacked) {
    console.error('该文件标记为 unpacked，真实内容在 app.asar.unpacked/ 下');
    process.exit(1);
  }
  const abs = contentStart + Number(node.offset);
  const buf = Buffer.alloc(node.size);
  fs.readSync(fd, buf, 0, node.size, abs);
  console.error('size=' + node.size + ' offset=' + node.offset + ' abs=' + abs);
  if (cmd === 'verify') {
    console.error('--- 尾部 200 字节 ---');
    console.error(buf.slice(Math.max(0, node.size - 200)).toString('utf8'));
  } else {
    const outPath = a2 || path.basename(a1);
    fs.writeFileSync(outPath, buf);
    console.error('已写出: ' + outPath + ' (' + buf.length + ' 字节)');
  }
} else if (cmd === 'extractall') {
  // a1 = 内部前缀（如 /out），a2 = 输出目录
  const prefix = a1.replace(/\/$/, '');
  const outDir = a2;
  if (!prefix || !outDir) { console.error('extractall 需要 <内部前缀> <输出目录>'); process.exit(2); }
  const all = collect(nodeAt(header, prefix) || { files: {} }, prefix, []);
  let ok = 0, skipped = 0;
  for (const [full, node] of all) {
    if (node.unpacked) { skipped++; continue; }
    const rel = full.slice(prefix.length).replace(/^\//, '');
    const dst = path.join(outDir, rel);
    fs.mkdirSync(path.dirname(dst), { recursive: true });
    const buf = Buffer.alloc(node.size);
    fs.readSync(fd, buf, 0, node.size, contentStart + Number(node.offset));
    fs.writeFileSync(dst, buf);
    ok++;
  }
  console.error('抽取完成: %d 个文件（跳过 unpacked %d 个） -> %s', ok, skipped, outDir);
} else {
  console.error('未知命令 ' + cmd);
  process.exit(2);
}
fs.closeSync(fd);
