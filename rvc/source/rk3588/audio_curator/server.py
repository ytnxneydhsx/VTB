#!/usr/bin/env python3

import argparse
import csv
import hashlib
import io
import json
import mimetypes
import sqlite3
import struct
import threading
from datetime import datetime, timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, quote, unquote, urlparse


VALID_STATUSES = {"unreviewed", "keep", "reject", "unsure"}
MAX_BODY_BYTES = 64 * 1024


def utc_now():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def stable_item_id(relative_path):
    return hashlib.sha1(relative_path.encode("utf-8")).hexdigest()[:20]


def logical_key(file_name):
    stem = Path(file_name).stem
    if stem.endswith(" (2)") or stem.endswith(" (3)"):
        stem = stem[:-4]
    return stem


def read_wav_metadata(path):
    with path.open("rb") as source:
        header = source.read(12)
        if len(header) != 12 or header[:4] not in {b"RIFF", b"RIFX"} or header[8:12] != b"WAVE":
            raise ValueError("not a RIFF/WAVE file")

        endian = "<"
        if header[:4] == b"RIFX":
            endian = ">"

        wav_format = None
        data_size = None
        while True:
            chunk_header = source.read(8)
            if len(chunk_header) < 8:
                break
            chunk_id, chunk_size = struct.unpack(endian + "4sI", chunk_header)
            payload = source.read(chunk_size)
            if chunk_size & 1:
                source.read(1)
            if chunk_id == b"fmt " and len(payload) >= 16:
                wav_format = struct.unpack(endian + "HHIIHH", payload[:16])
            if chunk_id == b"data":
                data_size = chunk_size
            if wav_format is not None and data_size is not None:
                break

        if wav_format is None or data_size is None:
            raise ValueError("missing fmt or data chunk")

        format_tag, channels, sample_rate, _, block_align, bits = wav_format
        duration = 0.0
        if sample_rate > 0 and block_align > 0:
            duration = data_size / (sample_rate * block_align)
        return {
            "format_tag": format_tag,
            "sample_rate": sample_rate,
            "channels": channels,
            "bits": bits,
            "duration": duration,
        }


def format_label(format_tag, sample_rate, channels, bits):
    format_name = "PCM"
    if format_tag == 3:
        format_name = "Float"
    channel_name = f"{channels}ch"
    if channels == 1:
        channel_name = "Mono"
    if channels == 2:
        channel_name = "Stereo"
    return f"{sample_rate / 1000:g}kHz · {channel_name} · {bits}-bit {format_name}"


class ReviewStore:
    def __init__(self, database_path, dataset_root):
        self.database_path = Path(database_path)
        self.dataset_root = Path(dataset_root).resolve()
        self._scan_lock = threading.Lock()
        self.database_path.parent.mkdir(parents=True, exist_ok=True)
        self._initialize()

    def connect(self):
        connection = sqlite3.connect(self.database_path, timeout=30)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA journal_mode=WAL")
        connection.execute("PRAGMA foreign_keys=ON")
        return connection

    def _initialize(self):
        with self.connect() as connection:
            connection.executescript(
                """
                CREATE TABLE IF NOT EXISTS items (
                    id TEXT PRIMARY KEY,
                    relative_path TEXT NOT NULL UNIQUE,
                    file_name TEXT NOT NULL,
                    logical_key TEXT NOT NULL,
                    split TEXT NOT NULL,
                    format_tag INTEGER NOT NULL,
                    sample_rate INTEGER NOT NULL,
                    channels INTEGER NOT NULL,
                    bits INTEGER NOT NULL,
                    duration REAL NOT NULL,
                    size_bytes INTEGER NOT NULL,
                    modified_ns INTEGER NOT NULL
                );

                CREATE TABLE IF NOT EXISTS reviews (
                    item_id TEXT PRIMARY KEY REFERENCES items(id) ON DELETE CASCADE,
                    status TEXT NOT NULL,
                    note TEXT NOT NULL DEFAULT '',
                    updated_at TEXT NOT NULL
                );

                CREATE INDEX IF NOT EXISTS idx_items_path ON items(relative_path);
                CREATE INDEX IF NOT EXISTS idx_items_logical_key ON items(logical_key);
                CREATE INDEX IF NOT EXISTS idx_reviews_status ON reviews(status);
                """
            )

    def scan(self):
        with self._scan_lock:
            rows = []
            errors = []
            for path in sorted(self.dataset_root.rglob("*.wav")):
                try:
                    relative_path = path.relative_to(self.dataset_root).as_posix()
                    metadata = read_wav_metadata(path)
                    stat = path.stat()
                    parts = relative_path.split("/")
                    split = "unknown"
                    if "train" in parts:
                        split = "train"
                    if "val" in parts:
                        split = "val"
                    rows.append(
                        (
                            stable_item_id(relative_path),
                            relative_path,
                            path.name,
                            logical_key(path.name),
                            split,
                            metadata["format_tag"],
                            metadata["sample_rate"],
                            metadata["channels"],
                            metadata["bits"],
                            metadata["duration"],
                            stat.st_size,
                            stat.st_mtime_ns,
                        )
                    )
                except (OSError, ValueError, struct.error) as error:
                    errors.append({"path": str(path), "error": str(error)})

            with self.connect() as connection:
                connection.executemany(
                    """
                    INSERT INTO items (
                        id, relative_path, file_name, logical_key, split,
                        format_tag, sample_rate, channels, bits, duration,
                        size_bytes, modified_ns
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                    ON CONFLICT(relative_path) DO UPDATE SET
                        file_name=excluded.file_name,
                        logical_key=excluded.logical_key,
                        split=excluded.split,
                        format_tag=excluded.format_tag,
                        sample_rate=excluded.sample_rate,
                        channels=excluded.channels,
                        bits=excluded.bits,
                        duration=excluded.duration,
                        size_bytes=excluded.size_bytes,
                        modified_ns=excluded.modified_ns
                    """,
                    rows,
                )
            return {"indexed": len(rows), "errors": errors}

    def summary(self):
        with self.connect() as connection:
            result = connection.execute(
                """
                SELECT
                    COUNT(*) AS total,
                    SUM(CASE WHEN COALESCE(reviews.status, 'unreviewed') = 'unreviewed' THEN 1 ELSE 0 END) AS unreviewed,
                    SUM(CASE WHEN reviews.status = 'keep' THEN 1 ELSE 0 END) AS keep_count,
                    SUM(CASE WHEN reviews.status = 'reject' THEN 1 ELSE 0 END) AS reject_count,
                    SUM(CASE WHEN reviews.status = 'unsure' THEN 1 ELSE 0 END) AS unsure_count,
                    SUM(items.duration) AS total_duration,
                    SUM(CASE WHEN reviews.status = 'keep' THEN items.duration ELSE 0 END) AS keep_duration
                FROM items
                LEFT JOIN reviews ON reviews.item_id = items.id
                """
            ).fetchone()
            return dict(result)

    def list_items(self, status, split, audio_format, search, offset, limit):
        conditions = []
        parameters = []
        if status != "all":
            conditions.append("COALESCE(reviews.status, 'unreviewed') = ?")
            parameters.append(status)
        if split != "all":
            conditions.append("items.split = ?")
            parameters.append(split)
        if audio_format == "pcm44100":
            conditions.append("items.format_tag = 1 AND items.sample_rate = 44100")
        if audio_format == "float22050":
            conditions.append("items.format_tag = 3 AND items.sample_rate = 22050")
        if search:
            conditions.append("(items.file_name LIKE ? OR items.relative_path LIKE ? OR reviews.note LIKE ?)")
            query = f"%{search}%"
            parameters.extend([query, query, query])

        where_sql = ""
        if conditions:
            where_sql = "WHERE " + " AND ".join(conditions)

        with self.connect() as connection:
            count_row = connection.execute(
                f"""
                SELECT COUNT(*) AS count
                FROM items
                LEFT JOIN reviews ON reviews.item_id = items.id
                {where_sql}
                """,
                parameters,
            ).fetchone()
            rows = connection.execute(
                f"""
                SELECT items.*, COALESCE(reviews.status, 'unreviewed') AS status,
                       COALESCE(reviews.note, '') AS note,
                       reviews.updated_at,
                       (SELECT COUNT(*) FROM items related WHERE related.logical_key = items.logical_key) AS related_count
                FROM items
                LEFT JOIN reviews ON reviews.item_id = items.id
                {where_sql}
                ORDER BY items.relative_path COLLATE NOCASE
                LIMIT ? OFFSET ?
                """,
                [*parameters, limit, offset],
            ).fetchall()

        items = []
        for row in rows:
            item = dict(row)
            item["format_label"] = format_label(
                item["format_tag"], item["sample_rate"], item["channels"], item["bits"]
            )
            item["audio_url"] = "/audio/" + quote(item["relative_path"], safe="/")
            items.append(item)
        return {"items": items, "total": count_row["count"], "offset": offset, "limit": limit}

    def update_review(self, item_id, status, note):
        if status not in VALID_STATUSES:
            raise ValueError("invalid status")
        note = note.strip()[:1000]
        with self.connect() as connection:
            exists = connection.execute("SELECT 1 FROM items WHERE id = ?", (item_id,)).fetchone()
            if exists is None:
                raise KeyError(item_id)
            if status == "unreviewed" and not note:
                connection.execute("DELETE FROM reviews WHERE item_id = ?", (item_id,))
            else:
                connection.execute(
                    """
                    INSERT INTO reviews(item_id, status, note, updated_at)
                    VALUES (?, ?, ?, ?)
                    ON CONFLICT(item_id) DO UPDATE SET
                        status=excluded.status,
                        note=excluded.note,
                        updated_at=excluded.updated_at
                    """,
                    (item_id, status, note, utc_now()),
                )
        return {"id": item_id, "status": status, "note": note}

    def export_rows(self, status):
        with self.connect() as connection:
            rows = connection.execute(
                """
                SELECT items.relative_path, items.duration, items.sample_rate,
                       items.channels, items.bits, reviews.status, reviews.note,
                       reviews.updated_at
                FROM reviews
                JOIN items ON items.id = reviews.item_id
                WHERE reviews.status = ?
                ORDER BY items.relative_path COLLATE NOCASE
                """,
                (status,),
            ).fetchall()
        return [dict(row) for row in rows]


class CuratorHandler(BaseHTTPRequestHandler):
    server_version = "AudioCurator/1.0"

    @property
    def review_store(self):
        return self.server.review_store

    @property
    def static_root(self):
        return self.server.static_root

    def log_message(self, message_format, *args):
        print(f"[{self.log_date_time_string()}] {self.address_string()} {message_format % args}")

    def send_json(self, payload, status=HTTPStatus.OK):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def read_json(self):
        content_length = int(self.headers.get("Content-Length", "0"))
        if content_length <= 0 or content_length > MAX_BODY_BYTES:
            raise ValueError("invalid request body size")
        return json.loads(self.rfile.read(content_length).decode("utf-8"))

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/api/summary":
            self.send_json(self.review_store.summary())
            return
        if parsed.path == "/api/items":
            self.handle_list_items(parsed)
            return
        if parsed.path == "/api/export":
            self.handle_export(parsed)
            return
        if parsed.path.startswith("/audio/"):
            self.handle_audio(parsed.path[len("/audio/") :])
            return
        self.handle_static(parsed.path)

    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path.startswith("/api/items/") and parsed.path.endswith("/review"):
            item_id = parsed.path[len("/api/items/") : -len("/review")].strip("/")
            try:
                payload = self.read_json()
                result = self.review_store.update_review(
                    item_id,
                    str(payload.get("status", "")),
                    str(payload.get("note", "")),
                )
                self.send_json(result)
            except KeyError:
                self.send_json({"error": "item not found"}, HTTPStatus.NOT_FOUND)
            except (ValueError, json.JSONDecodeError) as error:
                self.send_json({"error": str(error)}, HTTPStatus.BAD_REQUEST)
            return
        self.send_json({"error": "not found"}, HTTPStatus.NOT_FOUND)

    def handle_list_items(self, parsed):
        query = parse_qs(parsed.query)
        status = query.get("status", ["all"])[0]
        split = query.get("split", ["all"])[0]
        audio_format = query.get("format", ["all"])[0]
        search = query.get("search", [""])[0].strip()[:200]
        try:
            offset = max(0, int(query.get("offset", ["0"])[0]))
            limit = min(200, max(1, int(query.get("limit", ["100"])[0])))
        except ValueError:
            self.send_json({"error": "invalid pagination"}, HTTPStatus.BAD_REQUEST)
            return
        if status != "all" and status not in VALID_STATUSES:
            self.send_json({"error": "invalid status filter"}, HTTPStatus.BAD_REQUEST)
            return
        self.send_json(self.review_store.list_items(status, split, audio_format, search, offset, limit))

    def handle_export(self, parsed):
        query = parse_qs(parsed.query)
        status = query.get("status", ["keep"])[0]
        output_format = query.get("format", ["json"])[0]
        if status not in VALID_STATUSES - {"unreviewed"}:
            self.send_json({"error": "invalid export status"}, HTTPStatus.BAD_REQUEST)
            return
        rows = self.review_store.export_rows(status)
        if output_format == "txt":
            body = "".join(row["relative_path"] + "\n" for row in rows).encode("utf-8")
            content_type = "text/plain; charset=utf-8"
            extension = "txt"
        elif output_format == "csv":
            output = io.StringIO()
            field_names = [
                "relative_path",
                "duration",
                "sample_rate",
                "channels",
                "bits",
                "status",
                "note",
                "updated_at",
            ]
            writer = csv.DictWriter(output, fieldnames=field_names)
            writer.writeheader()
            writer.writerows(rows)
            body = output.getvalue().encode("utf-8-sig")
            content_type = "text/csv; charset=utf-8"
            extension = "csv"
        else:
            body = json.dumps(rows, ensure_ascii=False, indent=2).encode("utf-8")
            content_type = "application/json; charset=utf-8"
            extension = "json"
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Disposition", f'attachment; filename="keqing-{status}.{extension}"')
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def handle_audio(self, encoded_path):
        relative_path = unquote(encoded_path)
        try:
            audio_path = (self.review_store.dataset_root / relative_path).resolve()
            audio_path.relative_to(self.review_store.dataset_root)
        except (ValueError, OSError):
            self.send_error(HTTPStatus.FORBIDDEN)
            return
        if not audio_path.is_file() or audio_path.suffix.lower() != ".wav":
            self.send_error(HTTPStatus.NOT_FOUND)
            return

        file_size = audio_path.stat().st_size
        start = 0
        end = file_size - 1
        response_status = HTTPStatus.OK
        range_header = self.headers.get("Range")
        if range_header:
            try:
                unit, value = range_header.split("=", 1)
                if unit != "bytes" or "," in value:
                    raise ValueError
                start_text, end_text = value.split("-", 1)
                if start_text:
                    start = int(start_text)
                if end_text:
                    end = int(end_text)
                if not start_text:
                    suffix_length = int(end_text)
                    start = max(0, file_size - suffix_length)
                    end = file_size - 1
                if start < 0 or end < start or start >= file_size:
                    raise ValueError
                end = min(end, file_size - 1)
                response_status = HTTPStatus.PARTIAL_CONTENT
            except ValueError:
                self.send_response(HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE)
                self.send_header("Content-Range", f"bytes */{file_size}")
                self.end_headers()
                return

        content_length = end - start + 1
        self.send_response(response_status)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(content_length))
        if response_status == HTTPStatus.PARTIAL_CONTENT:
            self.send_header("Content-Range", f"bytes {start}-{end}/{file_size}")
        self.send_header("Cache-Control", "private, max-age=3600")
        self.end_headers()

        with audio_path.open("rb") as source:
            source.seek(start)
            remaining = content_length
            while remaining > 0:
                chunk = source.read(min(64 * 1024, remaining))
                if not chunk:
                    break
                self.wfile.write(chunk)
                remaining -= len(chunk)

    def handle_static(self, request_path):
        relative = "index.html"
        if request_path not in {"", "/"}:
            relative = request_path.lstrip("/")
        try:
            path = (self.static_root / relative).resolve()
            path.relative_to(self.static_root)
        except (ValueError, OSError):
            self.send_error(HTTPStatus.FORBIDDEN)
            return
        if not path.is_file():
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        content = path.read_bytes()
        content_type = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
        if content_type.startswith("text/") or content_type in {"application/javascript", "application/json"}:
            content_type += "; charset=utf-8"
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(content)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(content)


def parse_arguments():
    parser = argparse.ArgumentParser(description="Review and label an extracted voice dataset in a browser.")
    parser.add_argument("--dataset-root", required=True, help="Directory containing WAV files")
    parser.add_argument("--database", required=True, help="SQLite review database")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    script_root = Path(__file__).resolve().parent
    static_root = (script_root / "static").resolve()
    review_store = ReviewStore(arguments.database, arguments.dataset_root)
    scan_result = review_store.scan()
    print(f"Indexed {scan_result['indexed']} WAV files; errors: {len(scan_result['errors'])}")
    for error in scan_result["errors"][:10]:
        print(f"Scan error: {error['path']}: {error['error']}")

    server = ThreadingHTTPServer((arguments.host, arguments.port), CuratorHandler)
    server.review_store = review_store
    server.static_root = static_root
    print(f"Audio curator listening on http://{arguments.host}:{arguments.port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
