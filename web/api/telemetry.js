// Relay de telemetría entre el ESP32 y la página.
//   POST /api/telemetry            -> el ESP32 envía su estado (header x-device-key)
//   GET  /api/telemetry[?history=1] -> la página lee el último estado (y el historial)
// Se guarda en Upstash Redis usando su API REST, sin dependencias.

import { createHash, timingSafeEqual } from 'node:crypto';

const KEY_LATEST = 'potab:latest';
const KEY_HISTORY = 'potab:history';
const HISTORY_LEN = 300;          // ~25 min con un envío cada 5 s
const MAX_BODY_BYTES = 4096;

function redisConfig() {
  const url = process.env.KV_REST_API_URL || process.env.UPSTASH_REDIS_REST_URL;
  const token = process.env.KV_REST_API_TOKEN || process.env.UPSTASH_REDIS_REST_TOKEN;
  return url && token ? { url: url.replace(/\/$/, ''), token } : null;
}

async function redisPipeline(cfg, commands) {
  const r = await fetch(`${cfg.url}/pipeline`, {
    method: 'POST',
    headers: { Authorization: `Bearer ${cfg.token}`, 'Content-Type': 'application/json' },
    body: JSON.stringify(commands),
  });
  if (!r.ok) throw new Error(`Redis respondió ${r.status}`);
  const out = await r.json();
  const failed = out.find(x => x.error);
  if (failed) throw new Error(`Redis: ${failed.error}`);
  return out.map(x => x.result);
}

// Compara claves en tiempo constante (se hashean para igualar longitudes)
function sameKey(a, b) {
  const ha = createHash('sha256').update(String(a)).digest();
  const hb = createHash('sha256').update(String(b)).digest();
  return timingSafeEqual(ha, hb);
}

const num = v => (typeof v === 'number' && Number.isFinite(v) ? v : null);
const bool = v => v === true;
const text = v => (typeof v === 'string' ? v.slice(0, 60) : '');

function tank(t) {
  if (!t || typeof t !== 'object') return null;
  return { d: num(t.d), n: num(t.n), l: num(t.l), f: bool(t.f) };
}

// Solo se guardan los campos conocidos del firmware; el resto se descarta.
function sanitize(body) {
  if (!body || typeof body !== 'object' || Array.isArray(body)) return null;
  const t1 = tank(body.t1), t2 = tank(body.t2);
  if (!t1 || !t2 || typeof body.ev1 !== 'boolean' || typeof body.ev2 !== 'boolean') return null;
  return {
    t1, t2,
    ph: num(body.ph), phv: num(body.phv), phok: bool(body.phok),
    ev1: body.ev1, ev2: body.ev2, m1: text(body.m1), m2: text(body.m2),
    manual: bool(body.manual), listo: bool(body.listo),
    bat: num(body.bat), batp: bool(body.batp), batb: bool(body.batb),
    up: num(body.up),
  };
}

function parseBody(req) {
  if (typeof req.body === 'string') {
    try { return JSON.parse(req.body); } catch { return null; }
  }
  return req.body ?? null;
}

async function handlePost(req, res, cfg) {
  const deviceKey = process.env.DEVICE_KEY;
  if (!deviceKey) return res.status(500).json({ error: 'Falta la variable DEVICE_KEY en Vercel' });
  if (!sameKey(req.headers['x-device-key'] || '', deviceKey)) return res.status(401).json({ error: 'Clave de dispositivo incorrecta' });

  const length = Number(req.headers['content-length'] || 0);
  if (length > MAX_BODY_BYTES) return res.status(413).json({ error: 'Cuerpo demasiado grande' });

  const data = sanitize(parseBody(req));
  if (!data) return res.status(400).json({ error: 'JSON inválido: faltan t1, t2, ev1 o ev2' });

  const ts = Date.now();
  const latest = { ...data, ts };
  const point = { ts, n1: data.t1.n, n2: data.t2.n, ph: data.ph, ev1: data.ev1, ev2: data.ev2 };
  await redisPipeline(cfg, [
    ['SET', KEY_LATEST, JSON.stringify(latest)],
    ['LPUSH', KEY_HISTORY, JSON.stringify(point)],
    ['LTRIM', KEY_HISTORY, '0', String(HISTORY_LEN - 1)],
  ]);
  return res.status(200).json({ ok: true, ts });
}

async function handleGet(req, res, cfg) {
  const wantHistory = new URL(req.url, 'http://localhost').searchParams.get('history') === '1';
  const commands = [['GET', KEY_LATEST]];
  if (wantHistory) commands.push(['LRANGE', KEY_HISTORY, '0', String(HISTORY_LEN - 1)]);
  const [rawLatest, rawHistory] = await redisPipeline(cfg, commands);

  const latest = rawLatest ? JSON.parse(rawLatest) : null;
  const body = { latest, ageSec: latest ? Math.max(0, (Date.now() - latest.ts) / 1000) : null };
  if (wantHistory) body.history = (rawHistory || []).map(s => JSON.parse(s)).reverse();
  return res.status(200).json(body);
}

export default async function handler(req, res) {
  res.setHeader('Cache-Control', 'no-store');
  const cfg = redisConfig();
  if (!cfg) return res.status(500).json({ error: 'Falta conectar Upstash Redis (KV_REST_API_URL / KV_REST_API_TOKEN)' });

  try {
    if (req.method === 'POST') return await handlePost(req, res, cfg);
    if (req.method === 'GET') return await handleGet(req, res, cfg);
    res.setHeader('Allow', 'GET, POST');
    return res.status(405).json({ error: 'Método no permitido' });
  } catch (err) {
    return res.status(502).json({ error: 'No se pudo acceder a Redis: ' + err.message });
  }
}
