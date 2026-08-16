#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Modbus TCP 测试工具（esp32_modbus 项目专用）
==============================================

单文件工具，仅使用 Python 标准库，无需安装任何第三方依赖。
启动后自动打开浏览器页面，通过网页读写 Modbus TCP 从站。

用法：
    python3 tools/modbus_tool.py                 # 默认端口 8000，自动打开浏览器
    python3 tools/modbus_tool.py --port 9000     # 指定端口
    python3 tools/modbus_tool.py --no-browser    # 不自动打开浏览器，手动访问 http://127.0.0.1:8000

支持：
    - 功能码 01/02/03/04/05/06/0F/10（读线圈/离散输入/保持/输入寄存器，写单/多线圈与寄存器）
    - 明文 502 与 TLS 802（自签名证书忽略校验）
    - 自动轮询刷新（对急停/复位等输入监控特别有用）
    - 本项目的寄存器映射预设（DI/DO/设备信息/保持寄存器）
"""

import argparse
import json
import socket
import ssl
import struct
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ---------------------------------------------------------------------------
# Modbus TCP 客户端（连接缓存 + 互斥串行化）
# ---------------------------------------------------------------------------

class ModbusTcpClient:
    """懒连接 + 连接缓存；同一 ip/port/tls 复用连接，参数变化或失败时重连。"""

    def __init__(self, idle_timeout=30):
        self.lock = threading.Lock()
        self.sock = None
        self.tls_wrap = False
        self.key = None
        self.last_used = 0.0
        self.idle_timeout = idle_timeout

    def _connect(self, ip, port, use_tls):
        raw = socket.create_connection((ip, port), timeout=4)
        raw.settimeout(5)
        if use_tls:
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE          # 自签名证书，忽略校验
            s = ctx.wrap_socket(raw, server_hostname=ip)
        else:
            s = raw
        self.sock = s
        self.tls_wrap = use_tls
        self.key = (ip, port, use_tls)

    def _ensure(self, ip, port, use_tls):
        """确保连接可用；key 变化或 socket 已死则重连。"""
        key = (ip, port, use_tls)
        if self.sock is None or self.key != key:
            self._close_locked()
            self._connect(ip, port, use_tls)
            return
        # 探测连接是否还活着（select 非阻塞读，无数据=正常）
        import select
        r, _, _ = select.select([self.sock], [], [], 0)
        if r:
            try:
                if self.sock.recv(1, socket.MSG_PEEK) == b'':
                    raise ConnectionError('peer closed')
            except (ConnectionError, OSError):
                self._close_locked()
                self._connect(ip, port, use_tls)

    def _close_locked(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None
        self.key = None

    def transaction(self, ip, port, use_tls, uid, pdu, timeout=5.0):
        """发一条 Modbus 请求，返回完整响应字节（MBAP+PDU）。"""
        with self.lock:
            self._ensure(ip, port, use_tls)
            self.sock.settimeout(timeout)
            tid = int(time.time() * 1000) & 0xFFFF
            mbap = struct.pack('>HHHB', tid, 0, len(pdu) + 1, uid & 0xFF)
            self.sock.sendall(mbap + pdu)
            # 读 MBAP 头 7 字节
            head = b''
            while len(head) < 7:
                chunk = self.sock.recv(7 - len(head))
                if not chunk:
                    raise ConnectionError('connection closed by peer')
                head += chunk
            r_tid, proto, length, r_uid = struct.unpack('>HHHB', head)
            if length < 1 or length > 255:
                raise ValueError('bad response length %d' % length)
            body = b''
            while len(body) < length - 1:      # length 含 uid 1 字节
                chunk = self.sock.recv(length - 1 - len(body))
                if not chunk:
                    raise ConnectionError('connection closed by peer')
                body += chunk
            self.last_used = time.time()
            return head + body

    def housekeep(self):
        """空闲超时关闭连接（由 HTTP 线程周期调用）。"""
        with self.lock:
            if self.sock and time.time() - self.last_used > self.idle_timeout:
                self._close_locked()


_client = ModbusTcpClient()

# ---------------------------------------------------------------------------
# 请求/响应编解码
# ---------------------------------------------------------------------------

def build_read_pdu(fc, addr, qty):
    return bytes([fc, (addr >> 8) & 0xFF, addr & 0xFF,
                  (qty >> 8) & 0xFF, qty & 0xFF])

def parse_read_response(resp, qty=0):
    """resp 为完整 MBAP+PDU 响应。返回 (fc, 值列表)。异常时抛 ValueError。
    qty>0 时按请求数量截断位类型结果（去掉填充位）。"""
    fc = resp[7]
    if fc & 0x80:
        code = resp[8] if len(resp) > 8 else 0
        raise ValueError('Modbus 异常 0x%02X（%s）' % (
            code, EXC_NAMES.get(code, '未知')))
    if fc in (0x01, 0x02):
        byte_cnt = resp[8]
        data = resp[9:9 + byte_cnt]
        vals = [(data[i // 8] >> (i % 8)) & 1 for i in range(byte_cnt * 8)]
        return fc, vals[:qty] if qty else vals
    if fc in (0x03, 0x04):
        byte_cnt = resp[8]
        data = resp[9:9 + byte_cnt]
        vals = []
        for i in range(0, len(data), 2):
            vals.append((data[i] << 8) | data[i + 1])
        return fc, vals
    raise ValueError('未支持的功能码 0x%02X' % fc)

EXC_NAMES = {
    0x01: '非法功能码', 0x02: '非法数据地址', 0x03: '非法数据值',
    0x04: '从站设备故障', 0x05: '确认', 0x06: '从站设备忙',
    0x0A: '网关路径不可用', 0x0B: '网关目标设备无响应',
}

def build_write_pdu(fc, addr, values):
    if fc == 0x05:   # 写单线圈
        v = 0xFF00 if values[0] else 0x0000
        return bytes([0x05, (addr >> 8) & 0xFF, addr & 0xFF,
                      (v >> 8) & 0xFF, v & 0xFF])
    if fc == 0x06:   # 写单寄存器
        v = int(values[0]) & 0xFFFF
        return bytes([0x06, (addr >> 8) & 0xFF, addr & 0xFF,
                      (v >> 8) & 0xFF, v & 0xFF])
    if fc == 0x0F:   # 写多线圈
        qty = len(values)
        byte_cnt = (qty + 7) // 8
        data = bytearray(byte_cnt)
        for i, v in enumerate(values):
            if v:
                data[i // 8] |= 1 << (i % 8)
        return bytes([0x0F, (addr >> 8) & 0xFF, addr & 0xFF,
                      (qty >> 8) & 0xFF, qty & 0xFF, byte_cnt]) + bytes(data)
    if fc == 0x10:   # 写多寄存器
        qty = len(values)
        data = bytearray()
        for v in values:
            v = int(v) & 0xFFFF
            data += bytes([(v >> 8) & 0xFF, v & 0xFF])
        return bytes([0x10, (addr >> 8) & 0xFF, addr & 0xFF,
                      (qty >> 8) & 0xFF, qty & 0xFF, qty * 2]) + bytes(data)
    raise ValueError('未支持的写功能码 0x%02X' % fc)

def parse_write_response(resp, fc):
    rfc = resp[7]
    if rfc & 0x80:
        code = resp[8] if len(resp) > 8 else 0
        raise ValueError('Modbus 异常 0x%02X（%s）' % (
            code, EXC_NAMES.get(code, '未知')))
    if fc == 0x05:
        addr = (resp[8] << 8) | resp[9]
        val = 'ON' if (resp[10] << 8 | resp[11]) == 0xFF00 else 'OFF'
        return '线圈 0x%04X = %s' % (addr, val)
    if fc == 0x06:
        addr = (resp[8] << 8) | resp[9]
        val = (resp[10] << 8) | resp[11]
        return '寄存器 0x%04X = 0x%04X (%d)' % (addr, val, val)
    if fc in (0x0F, 0x10):
        addr = (resp[8] << 8) | resp[9]
        qty = (resp[10] << 8) | resp[11]
        return '已写入 %d 个%s @ 0x%04X' % (
            qty, '线圈' if fc == 0x0F else '寄存器', addr)
    return '写入成功'

# ---------------------------------------------------------------------------
# HTTP 服务
# ---------------------------------------------------------------------------

PAGE = r'''<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<title>Modbus TCP 测试工具 - esp32_modbus</title>
<style>
  :root { --accent:#4f46e5; --ok:#16a34a; --err:#dc2626; --bg:#f5f6fa; }
  * { box-sizing:border-box; margin:0; padding:0; }
  body { font:14px/1.6 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif; background:var(--bg); color:#1f2937; }
  .wrap { max-width:1080px; margin:0 auto; padding:20px; }
  h1 { font-size:20px; margin-bottom:4px; }
  .sub { color:#6b7280; font-size:13px; margin-bottom:18px; }
  .card { background:#fff; border:1px solid #e5e7eb; border-radius:10px; padding:16px; margin-bottom:16px; }
  .card h2 { font-size:15px; margin-bottom:12px; color:var(--accent); }
  .row { display:flex; gap:10px; align-items:center; flex-wrap:wrap; }
  label { font-size:13px; color:#374151; }
  input, select { padding:6px 8px; border:1px solid #d1d5db; border-radius:6px; font-size:13px; background:#fff; }
  input[type=number] { width:90px; }
  input[type=text] { width:150px; }
  button { padding:6px 14px; border:none; border-radius:6px; background:var(--accent); color:#fff; font-size:13px; cursor:pointer; }
  button:hover { opacity:.9; }
  button.gray { background:#6b7280; }
  button.green { background:var(--ok); }
  button.red { background:var(--err); }
  .status { display:inline-flex; align-items:center; gap:6px; padding:6px 12px; border-radius:6px; font-size:13px; background:#e5e7eb; color:#374151; }
  .status .dot { width:8px; height:8px; border-radius:50%; background:#9ca3af; }
  .status.ok { background:#dcfce7; color:#166534; } .status.ok .dot{ background:var(--ok); }
  .status.err { background:#fee2e2; color:#991b1b; } .status.err .dot{ background:var(--err); }
  .status.busy { background:#fef3c7; color:#92400e; } .status.busy .dot{ background:#f59e0b; }
  table { width:100%; border-collapse:collapse; font-size:13px; }
  th, td { padding:6px 10px; border:1px solid #e5e7eb; text-align:left; }
  th { background:#f9fafb; }
  td.num { font-family:ui-monospace,Menlo,monospace; }
  .presets { display:flex; gap:8px; flex-wrap:wrap; margin-bottom:10px; }
  .presets button { background:#eef2ff; color:#4338ca; border:1px solid #c7d2fe; }
  .presets button:hover { background:#e0e7ff; }
  .err-msg { color:var(--err); font-size:13px; margin-top:8px; min-height:18px; }
  .hint { color:#6b7280; font-size:12px; margin-top:6px; }
  .grid2 { display:grid; grid-template-columns:1fr 1fr; gap:16px; }
  @media (max-width:800px){ .grid2{ grid-template-columns:1fr; } }
</style>
</head>
<body>
<div class="wrap">
  <h1>Modbus TCP 测试工具</h1>
  <div class="sub">ESP32 Modbus 从站调试 · 支持 01/02/03/04/05/06/0F/10 · 明文与 TLS</div>

  <!-- 连接配置 -->
  <div class="card">
    <h2>🔌 连接配置</h2>
    <div class="row">
      <label>设备 IP <input type="text" id="ip" value="192.168.0.175" placeholder="如 192.168.0.175"></label>
      <label>端口 <input type="number" id="port" value="502" min="1" max="65535"></label>
      <label><input type="checkbox" id="tls"> TLS</label>
      <label>单元号 <input type="number" id="uid" value="1" min="0" max="247"></label>
      <button id="btnTest" class="gray">测试连接</button>
      <span class="status" id="status"><span class="dot"></span><span id="statusText">未连接</span></span>
    </div>
    <div class="err-msg" id="connErr"></div>
  </div>

  <!-- 读操作 -->
  <div class="card">
    <h2>📖 读取</h2>
    <div class="presets">
      <button onclick="preset(2,0x1000,7)">读全部 DI（急停/复位）</button>
      <button onclick="preset(1,0x0000,4)">读 DO 线圈</button>
      <button onclick="preset(4,0x3000,4)">读设备信息</button>
      <button onclick="preset(3,0x4000,4)">读保持寄存器</button>
    </div>
    <div class="row">
      <label>功能码
        <select id="rfc">
          <option value="1">01 读线圈</option>
          <option value="2" selected>02 读离散输入</option>
          <option value="3">03 读保持寄存器</option>
          <option value="4">04 读输入寄存器</option>
        </select>
      </label>
      <label>起始地址 <input type="text" id="raddr" value="0x1000"></label>
      <label>数量 <input type="number" id="rqty" value="7" min="1" max="125"></label>
      <button id="btnRead">读取</button>
      <label style="display:inline-flex;align-items:center;gap:4px;">
        <input type="checkbox" id="auto"> 自动刷新
      </label>
      <label>间隔 <input type="number" id="interval" value="1000" min="200" style="width:80px;"> ms</label>
    </div>
    <div style="margin-top:10px;">
      <table>
        <thead><tr><th style="width:120px;">地址</th><th style="width:100px;">值</th><th>说明</th></tr></thead>
        <tbody id="readRows"><tr><td colspan="3" style="color:#9ca3af;">点击"读取"或选择预设</td></tr></tbody>
      </table>
    </div>
    <div class="err-msg" id="readErr"></div>
  </div>

  <!-- 写操作 -->
  <div class="card">
    <h2>✏️ 写入</h2>
    <div class="grid2">
      <div>
        <div class="row" style="margin-bottom:10px;">
          <label>类型
            <select id="wtype">
              <option value="05">05 写单线圈</option>
              <option value="06">06 写单寄存器</option>
              <option value="0f">0F 写多线圈</option>
              <option value="10">10 写多寄存器</option>
            </select>
          </label>
          <label>起始地址 <input type="text" id="waddr" value="0x0000"></label>
        </div>
        <div class="row">
          <label>值
            <select id="wval1">
              <option value="1">ON / 1</option>
              <option value="0">OFF / 0</option>
            </select>
            <input type="text" id="wvals" placeholder="多值用逗号分隔，如 1,0,1" style="display:none;width:220px;">
          </label>
          <button id="btnWrite" class="green">写入</button>
        </div>
        <div class="hint">单线圈写 ON/OFF；多线圈/多寄存器用逗号分隔值（如 1,0,1 或 100,200）</div>
      </div>
      <div>
        <div class="row">
          <label>快速写 DO 测试</label>
          <button onclick="quickDo(0,1)">DO0 ON</button>
          <button onclick="quickDo(0,0)">DO0 OFF</button>
          <button onclick="quickDo(1,1)">DO1 ON</button>
          <button onclick="quickDo(1,0)">DO1 OFF</button>
        </div>
        <div class="err-msg" id="writeErr"></div>
      </div>
    </div>
  </div>

  <div class="sub">寄存器映射：线圈 0x0000（DO0-3）· 离散输入 0x1000（DI0-6，0x1004/05 急停锁存、0x1006 复位）· 输入寄存器 0x3000（设备信息）· 保持寄存器 0x4000（参数）</div>
</div>

<script>
var timer = null;
var $ = function(id){ return document.getElementById(id); };

function setStatus(cls, text){
  var s = $('status');
  s.className = 'status ' + cls;
  $('statusText').textContent = text;
}

function params(){
  var ipv = $('ip').value.trim();
  if (!ipv) {
    setStatus('err', '未填 IP');
    $('connErr').textContent = '❌ 请先在"设备 IP"框中输入设备地址（如 192.168.0.175）';
    return null;
  }
  return {
    ip: ipv,
    port: parseInt($('port').value, 10) || 502,
    tls: $('tls').checked,
    uid: parseInt($('uid').value, 10) || 1
  };
}

function post(url, body){
  return fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body)
  }).then(function(r){ return r.json(); });
}

/* ---- 地址解析：支持 0x 前缀十六进制 ---- */
function parseAddr(s){
  s = String(s).trim();
  return parseInt(s, s.toLowerCase().startsWith('0x') ? 16 : 10);
}

/* ---- 测试连接 ---- */
$('btnTest').addEventListener('click', function(){
  var p = params(); if (!p) return; p.fc = 4; p.addr = 0x3000; p.qty = 1;
  setStatus('busy', '测试中…');
  post('/api/read', p).then(function(d){
    if (d.ok) { setStatus('ok', '已连接 ' + p.ip + ':' + p.port + (p.tls?' (TLS)':'')); $('connErr').textContent=''; }
    else { setStatus('err', '连接失败'); $('connErr').textContent = '❌ ' + d.error; }
  }).catch(function(e){
    setStatus('err', '连接失败'); $('connErr').textContent = '❌ ' + e.message;
  });
});

/* ---- 读取 ---- */
function preset(fc, addr, qty){
  $('rfc').value = String(fc);
  $('raddr').value = '0x' + addr.toString(16).toUpperCase();
  $('rqty').value = qty;
  doRead();
}

function desc(idx, addr, val){
  var a = addr & 0xFFFF;
  var map = {
    0x1000:'DI0 通用输入', 0x1001:'DI1 通用输入', 0x1002:'DI2 通用输入', 0x1003:'DI3 通用输入',
    0x1004:'DI4 急停1（锁存）', 0x1005:'DI5 急停2（锁存）', 0x1006:'DI6 复位按钮', 0x1007:'DI7 未使用',
    0x0000:'DO0 输出', 0x0001:'DO1 输出', 0x0002:'DO2 输出', 0x0003:'DO3 输出',
    0x3000:'固件版本', 0x3001:'WiFi 状态', 0x3002:'运行秒数(低)', 0x3003:'运行秒数(高)',
    0x4000:'用户参数0', 0x4001:'用户参数1', 0x4002:'用户参数2', 0x4003:'用户参数3'
  };
  var key = a;
  if (map[key] !== undefined) return map[key];
  return (addr >= 0x1000 && addr < 0x1000 + 8) ? '离散输入' :
         (addr >= 0x0000 && addr < 0x0000 + 4) ? '线圈' :
         (addr >= 0x3000 && addr < 0x3000 + 4) ? '输入寄存器' :
         (addr >= 0x4000 && addr < 0x4000 + 4) ? '保持寄存器' : '—';
}

function doRead(){
  var p = params(); if (!p) return;
  p.fc = parseInt($('rfc').value, 10);
  p.addr = parseAddr($('raddr').value);
  p.qty = parseInt($('rqty').value, 10) || 1;
  if (isNaN(p.addr)) { $('readErr').textContent = '❌ 地址格式错误'; return; }
  $('readErr').textContent = '';
  post('/api/read', p).then(function(d){
    if (!d.ok) { $('readErr').textContent = '❌ ' + d.error; return; }
    var rows = '';
    for (var i = 0; i < d.values.length; i++){
      var addr = p.addr + i;
      var v = d.values[i];
      var vshow = (p.fc === 1 || p.fc === 2) ? (v ? '1' : '0')
                 : ('0x' + ('0000' + v.toString(16).toUpperCase()).slice(-4) + ' (' + v + ')');
      rows += '<tr><td class="num">0x' + ('0000' + addr.toString(16).toUpperCase()).slice(-4) + '</td>'
            + '<td class="num">' + vshow + '</td><td>' + desc(i, addr, v) + '</td></tr>';
    }
    $('readRows').innerHTML = rows;
  }).catch(function(e){ $('readErr').textContent = '❌ ' + e.message; });
}

$('btnRead').addEventListener('click', doRead);

/* ---- 自动刷新 ---- */
$('auto').addEventListener('change', function(){
  if (timer) { clearInterval(timer); timer = null; }
  if (this.checked) {
    var iv = parseInt($('interval').value, 10) || 1000;
    timer = setInterval(doRead, Math.max(200, iv));
  }
});

/* ---- 写入 ---- */
$('wtype').addEventListener('change', function(){
  var t = this.value;
  var single = (t === '05' || t === '06');
  $('wval1').style.display = single ? '' : 'none';
  $('wvals').style.display = single ? 'none' : '';
});

function doWrite(){
  var p = params(); if (!p) return;
  var fc = parseInt($('wtype').value, 16);
  p.fc = fc;
  p.addr = parseAddr($('waddr').value);
  if (isNaN(p.addr)) { $('writeErr').textContent = '❌ 地址格式错误'; return; }
  var vals;
  if (fc === 0x05) {
    vals = [$('wval1').value === '1' ? 1 : 0];
  } else if (fc === 0x06) {
    vals = [parseInt($('wval1').value, 10) || 0];
    if ($('wval1').selectedIndex < 0) vals = [parseAddr($('wval1').value) || 0];
  } else {
    var raw = $('wvals').value.trim();
    vals = raw ? raw.split(/[,，\s]+/).map(function(s){
      return /^0x/i.test(s) ? parseInt(s, 16) : parseInt(s, 10);
    }) : [];
    if (!vals.length || vals.some(isNaN)) { $('writeErr').textContent = '❌ 值格式错误'; return; }
  }
  p.values = vals;
  $('writeErr').textContent = '';
  post('/api/write', p).then(function(d){
    $('writeErr').textContent = d.ok ? ('✅ ' + d.message) : ('❌ ' + d.error);
  }).catch(function(e){ $('writeErr').textContent = '❌ ' + e.message; });
}

$('btnWrite').addEventListener('click', doWrite);

function quickDo(no, on){
  var p = params(); if (!p) return;
  p.fc = 5; p.addr = no; p.values = [on];
  post('/api/write', p).then(function(d){
    $('writeErr').textContent = d.ok ? ('✅ DO' + no + ' → ' + (on?'ON':'OFF') + '（' + d.message + '）') : ('❌ ' + d.error);
  }).catch(function(e){ $('writeErr').textContent = '❌ ' + e.message; });
}

/* 回车快捷键 */
document.addEventListener('keydown', function(e){
  if (e.key === 'Enter' && e.target.tagName !== 'BUTTON') doRead();
});
</script>
</body>
</html>
'''


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):        # 精简日志
        print('[http] %s %s' % (self.address_string(), fmt % args))

    def _send_json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_body(self):
        try:
            length = int(self.headers.get('Content-Length', 0))
        except ValueError:
            length = 0
        if length <= 0 or length > 1 << 20:
            return None
        return json.loads(self.rfile.read(length).decode('utf-8'))

    def do_GET(self):
        if self.path in ('/', '/index.html'):
            body = PAGE.encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self._send_json({'ok': False, 'error': 'not found'}, 404)

    def do_POST(self):
        body = self._read_body()
        if body is None:
            self._send_json({'ok': False, 'error': 'bad request'}, 400)
            return
        try:
            ip = str(body.get('ip', '')).strip()
            if not ip:
                raise ValueError('未填写设备 IP（页面顶部"设备 IP"框）')
            port = int(body['port'])
            tls = bool(body.get('tls'))
            uid = int(body.get('uid', 1)) & 0xFF
            fc = int(body['fc'])
            addr = int(body['addr']) & 0xFFFF
            if self.path == '/api/read':
                qty = int(body['qty'])
                if qty < 1 or qty > 125:
                    raise ValueError('数量需在 1~125')
                resp = _client.transaction(ip, port, tls, uid, build_read_pdu(fc, addr, qty))
                _, values = parse_read_response(resp, qty)
                self._send_json({'ok': True, 'values': values})
            elif self.path == '/api/write':
                values = body['values']
                resp = _client.transaction(ip, port, tls, uid, build_write_pdu(fc, addr, values))
                msg = parse_write_response(resp, fc)
                self._send_json({'ok': True, 'message': msg})
            else:
                self._send_json({'ok': False, 'error': 'not found'}, 404)
        except (KeyError, ValueError, TypeError) as e:
            self._send_json({'ok': False, 'error': str(e)})
        except (socket.timeout, TimeoutError):
            self._send_json({'ok': False, 'error': '请求超时（设备无响应）→ 目标 %s:%d%s。检查 IP 是否正确、设备是否在线' % (
                ip, port, ' (TLS)' if tls else '')})
        except ConnectionRefusedError:
            self._send_json({'ok': False, 'error': '连接被拒绝 → 目标 %s:%d%s 无监听。可能原因：① 设备正在重启/重连（等几秒再试）② Modbus 从站未启用（设备网页高级设置勾选"启用"）③ 端口填错（明文 502 / TLS 802）' % (
                ip, port, ' (TLS)' if tls else '')})
        except ConnectionResetError:
            self._send_json({'ok': False, 'error': '连接被重置 → 目标 %s:%d%s。端口与 TLS 设置不匹配（502 明文端口不勾 TLS；802 是 TLS 端口需勾选 TLS）' % (
                ip, port, ' (TLS)' if tls else '')})
        except (ConnectionError, OSError) as e:
            self._send_json({'ok': False, 'error': '连接失败 → 目标 %s:%d%s：%s' % (
                ip, port, ' (TLS)' if tls else '', e)})


def main():
    ap = argparse.ArgumentParser(description='Modbus TCP 测试工具')
    ap.add_argument('--port', type=int, default=8000, help='HTTP 端口（默认 8000）')
    ap.add_argument('--host', default='127.0.0.1', help='监听地址（默认 127.0.0.1）')
    ap.add_argument('--no-browser', action='store_true', help='不自动打开浏览器')
    args = ap.parse_args()

    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print('=' * 56)
    print('  Modbus TCP 测试工具已启动')
    print('  浏览器访问:  http://%s:%d' % (args.host, args.port))
    print('  按 Ctrl+C 退出')
    print('=' * 56)

    if not args.no_browser:
        webbrowser.open('http://%s:%d' % (args.host, args.port))

    # 后台线程：连接空闲清理
    def housekeep():
        while True:
            time.sleep(5)
            _client.housekeep()
    threading.Thread(target=housekeep, daemon=True).start()

    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print('\n退出')


if __name__ == '__main__':
    main()
