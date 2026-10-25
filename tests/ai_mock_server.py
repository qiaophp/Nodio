#!/usr/bin/env python3
# tests/ai_mock_server.py — OpenAI 兼容接口的**本地 mock**（只依赖标准库）。
#
# 用途：IDE 冒烟要验证 AI 面板的**成功回包**路径（面板把 AI 的回答写进
# 对话日志、并记进会话），而真实接口需要联网、还依赖用户的 Key。
# 这个 mock 让冒烟：
#   · 不依赖外网（127.0.0.1）
#   · 不碰用户真实凭据（隔离名，见 ide_smoke.ps1 的 NODIO_AI_CRED_TARGET）
#   · 返回固定的、可断言的内容
#
# 协议：
#   POST <任意路径>  → 200 + OpenAI 形状的 JSON
#                       （路径里带 "/fail" 的 → 500 + {"error":{"message":...}}）
#   GET  /ping        → 200 {"pong":true}（就绪探针）
#
# 启动：python tests/ai_mock_server.py [--parent-pid <pid>] [--max-requests N]
#   → 绑定 127.0.0.1:0（系统挑一个空闲端口），往 stdout 打一行
#     "READY <port>" 并 flush —— 父进程读这一行就知道端口和「已就绪」。
#
# 退出（按可靠性排序）：
#   ① --parent-pid 指定的父进程一消失就退出（看门狗）
#   ② --max-requests 用尽
#
# ⚠ 为什么必须有看门狗（①）：这个脚本是被 Start-Process -NoNewWindow 拉起来的，
#   它的 stdin 是**继承来的控制台/管道**，在自动化场景下**立刻就是 EOF**。
#   原来的「读 stdin 等退出」写法有两个坑：
#     · EOF 立刻到来 → shutdown() 在 serve_forever 真正开始前就置位
#       __shutdown_request → serve_forever 的 while 直接不进 → 服务器秒退，
#       表现为「READY 打印了，但一连就 Connection refused」。
#     · 反过来，若不立刻 EOF，父进程退出时又收不到 EOF → 孤儿进程一直挂着，
#       还占着控制台句柄 → 调用方的输出管道等不到 EOF，整个调用看起来「卡住」。
#   所以改成：父进程 PID + WaitForSingleObject，父进程一死就收摊，
#   再由 ide_smoke.ps1 的 finally 兜底 Kill 一次。
#
# ⚠ 为什么必须用 serve_forever 而不是手写 handle_request() 循环：
#   BaseServer.shutdown() 会等 __is_shut_down 事件，而那个事件**只有
#   serve_forever() 的 finally 才会置位**。手写 handle_request() 循环时调用
#   shutdown() 会永久阻塞 —— 看门狗线程挂死、主线程 join 超时。
import argparse
import ctypes
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

REPLY = "MOCK-OK 你好，这是 mock 服务器的回包"
MODEL = "mock-model"

MAX_REQUESTS = 4

srv = None          # 供 Handler / 看门狗访问（main() 里赋值）
_req_count = 0      # 已处理的请求数
_req_lock = threading.Lock()


def _bump_and_maybe_stop():
    """数一次请求；到上限就收摊（用独立线程调 shutdown，别卡住 handler）。"""
    global _req_count
    with _req_lock:
        _req_count += 1
        n = _req_count
    if n >= MAX_REQUESTS:
        threading.Thread(target=srv.shutdown, daemon=True).start()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"          # 每条响应后关闭，避免 keep-alive 纠缠

    def log_message(self, fmt, *args):     # 别把访问日志写进 stdout（那是协议通道）
        pass

    def handle_one_request(self):
        try:
            super().handle_one_request()
        finally:
            _bump_and_maybe_stop()

    def _send(self, code, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/ping"):
            self._send(200, {"pong": True})
        else:
            self._send(404, {"error": {"message": "not found"}})

    def do_POST(self):
        # 必须把请求体读完，否则客户端会收到 connection reset
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = 0
        if n > 0:
            self.rfile.read(n)

        if "/fail" in self.path:
            self._send(500, {"error": {"message": "mock failure", "type": "mock_error"}})
            return

        self._send(200, {
            "id": "chatcmpl-mock",
            "object": "chat.completion",
            "created": 0,
            "model": MODEL,
            "choices": [{
                "index": 0,
                "message": {"role": "assistant", "content": REPLY},
                "finish_reason": "stop",
            }],
            "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
        })


def watch_parent(pid):
    """父进程一死就关掉服务器。

    用 OpenProcess(SYNCHRONIZE) + WaitForSingleObject：内核会在父进程
    终止时把这个等待唤醒，不用轮询。

    ⚠ 千万别用 os.kill(pid, 0) 探活 —— Windows 上 os.kill 会直接
    TerminateProcess，那是把父进程杀了。
    """
    SYNCHRONIZE = 0x00100000
    INFINITE = 0xFFFFFFFF
    try:
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
        if not handle:
            # 拿不到句柄 = 父进程已经没了（或权限不对）→ 直接收摊
            srv.shutdown()
            return
        try:
            kernel32.WaitForSingleObject(handle, INFINITE)
        finally:
            kernel32.CloseHandle(handle)
    except Exception:
        # 看门狗自己出问题不该影响服务器本体，继续服务即可
        pass
    srv.shutdown()


def main():
    global srv, MAX_REQUESTS
    parser = argparse.ArgumentParser()
    parser.add_argument("--parent-pid", type=int, default=0,
                        help="父进程 PID；它退出后本服务器自动退出")
    parser.add_argument("--max-requests", type=int, default=MAX_REQUESTS)
    args = parser.parse_args()
    MAX_REQUESTS = args.max_requests

    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = srv.server_address[1]
    print("READY %d" % port, flush=True)

    # 收到 stdin EOF **不**作为退出条件（自动化下 stdin 立刻 EOF，
    # 会把服务秒退 —— 见文件头注释）。只留父进程看门狗 + 请求数上限。
    if args.parent_pid > 0:
        threading.Thread(target=watch_parent, args=(args.parent_pid,), daemon=True).start()

    # serve_forever 在主线程：shutdown() 必须能叫停它（见文件头注释）
    try:
        srv.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()


if __name__ == "__main__":
    main()
