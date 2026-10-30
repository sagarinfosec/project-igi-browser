<?php
// Crash reports from the page (loader.js crashCheck: a start that finds the previous run's heartbeat, i.e. the
// browser killed the page and reloaded it). One JSON line per report, appended to igi_crash.log in the folder
// ABOVE the web root (not served). No addresses or names are stored. ?view=KEY prints the last 300 lines; the
// key is the content of igi_crash.key next to the log (no key file: no viewing).
header('Cache-Control: no-store');
$log = dirname(__DIR__) . '/igi_crash.log';
$keyFile = dirname(__DIR__) . '/igi_crash.key';

if (isset($_GET['view'])) {
    header('Content-Type: text/plain; charset=utf-8');
    $key = trim((string) @file_get_contents($keyFile));
    if ($key === '' || !hash_equals($key, (string) $_GET['view'])) { http_response_code(403); echo "no\n"; exit; }
    $lines = @file($log);
    if (!$lines) { echo "no reports yet\n"; exit; }
    echo count($lines) . " reports, last 300:\n";
    echo implode('', array_slice($lines, -300));
    exit;
}

if ($_SERVER['REQUEST_METHOD'] !== 'POST') { http_response_code(405); exit; }
$body = file_get_contents('php://input', false, null, 0, 4096);
$d = json_decode((string) $body, true);
if (!is_array($d)) { http_response_code(400); exit; }
// rotate at 20 MB (one old file kept): the start / play events of the A/B cohorts make it grow ~10x faster
if (@filesize($log) > 20000000) { @rename($log, $log . '.1'); }
$row = array('at' => gmdate('Y-m-d H:i:s'));
// type: reload (the start after a killed page), hang / hangend (the page thread stopped answering the watchdog
// worker, and answered again), stall / stallend (frames stopped in a running mission); dur in seconds; pp/gp
// the page and game thread phases, frames/gf/beat/io counters, wasm the memory size in MB (host/win32.h RcWatch)
foreach (array('v', 'type', 'ua', 'mem', 'cores', 'played', 'gap', 'dur', 'mission', 'paused', 'touch', 'bg', 'nav', 'screen',
               'pp', 'gp', 'frames', 'queued', 'gf', 'beat', 'io', 'wasm', 'lt', 'fps', 'rb', 'msg', 'page', 'gpu', 'x', 'booted', 'play', 'expoff', 't') as $k) {
    if (!isset($d[$k])) continue;
    $row[$k] = is_string($d[$k]) ? substr($d[$k], 0, 300) : (is_numeric($d[$k]) ? $d[$k] + 0 : null);
}
@file_put_contents($log, json_encode($row) . "\n", FILE_APPEND | LOCK_EX);
http_response_code(204);
