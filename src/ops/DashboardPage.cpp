#include "theseed/ops/DashboardPage.h"

namespace theseed::ops {

// 单页 dashboard。设计约束：无外部资源（零 CDN/字体/图片）、无构建步骤、
// 只读（fetch 全部来自 OpsServer 既有端点）。告警由前端按上次轮询的指标
// 增量推导（slow_tick / backpressure / 队列深度越限 / 实体骤降），不做
// 中心告警库。
std::string_view dashboardHtml() {
    return R"dashboard(<!doctype html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>theseed ops dashboard</title>
<style>
:root{--bg:#0d1117;--panel:#161b22;--border:#30363d;--fg:#e6edf3;--dim:#8b949e;
--green:#3fb950;--red:#f85149;--yellow:#d29922;--blue:#58a6ff;}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);
font:14px/1.5 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
header{display:flex;align-items:baseline;gap:12px;padding:14px 20px;
border-bottom:1px solid var(--border)}
header h1{font-size:16px;margin:0;font-weight:600}
header .sub{color:var(--dim);font-size:12px}
main{padding:16px 20px;display:grid;gap:14px}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:14px}
.card{background:var(--panel);border:1px solid var(--border);border-radius:8px;padding:12px 14px}
.card .k{color:var(--dim);font-size:12px;margin-bottom:6px}
.card .v{font-size:24px;font-weight:700}
.card .s{color:var(--dim);font-size:11px;margin-top:4px}
.panel{background:var(--panel);border:1px solid var(--border);border-radius:8px;padding:12px 14px}
.panel h2{font-size:13px;margin:0 0 10px;color:var(--dim);font-weight:600}
table{width:100%;border-collapse:collapse;font-size:12px}
td,th{padding:4px 8px;text-align:left;border-bottom:1px solid var(--border)}
th{color:var(--dim);font-weight:500}
.alarm{padding:6px 10px;border-left:3px solid var(--red);background:#f8514911;
margin-bottom:6px;border-radius:0 6px 6px 0;font-size:12px}
.alarm.warn{border-left-color:var(--yellow);background:#d2992211}
.ok{color:var(--green)}.bad{color:var(--red)}
details summary{cursor:pointer;color:var(--dim);font-size:12px}
pre{margin:8px 0 0;max-height:320px;overflow:auto;font-size:11px;color:var(--dim)}
.err{color:var(--red);font-size:12px}
</style>
</head>
<body>
<header>
<h1>theseed ops dashboard</h1>
<span class="sub" id="role">-</span>
<span class="sub" id="uptime">-</span>
<span class="sub" id="poll">polling /metrics every 2s</span>
</header>
<main>
<div class="cards">
<div class="card"><div class="k">在线玩家 sessions</div><div class="v" id="sessions">-</div><div class="s" id="entities-types"></div></div>
<div class="card"><div class="k">实体数 entities</div><div class="v" id="entities">-</div><div class="s" id="entities-sub"></div></div>
<div class="card"><div class="k">tick 耗时 avg</div><div class="v" id="tick">-</div><div class="s" id="tick-sub"></div></div>
<div class="card"><div class="k">内存 RSS</div><div class="v" id="rss">-</div><div class="s">process_rss_bytes</div></div>
<div class="card"><div class="k">发送队列 queue_backlog</div><div class="v" id="qout">-</div><div class="s" id="qin"></div></div>
<div class="card"><div class="k">消息吞吐 msg/s</div><div class="v" id="thr">-</div><div class="s" id="thr-sub"></div></div>
</div>
<div class="panel"><h2>最近告警（指标增量推导）</h2><div id="alarms"><div class="alarm warn">等待首轮采样…</div></div></div>
<div class="panel"><h2>传输 / 登录明细</h2><table id="detail"></table></div>
<details><summary>原始 /metrics</summary><pre id="raw">-</pre></details>
</main>
<script>
"use strict";
const $=id=>document.getElementById(id);
let prev=null, prevAt=0, alarms=[];
function fmtBytes(n){if(!n&&n!==0)return"-";const u=["B","KiB","MiB","GiB"];let i=0,v=n;
while(v>=1024&&i<u.length-1){v/=1024;i++}return v.toFixed(1)+u[i]}
function txt(id,v){$(id).textContent=v}
// Prometheus 文本 -> {name: value}（取每名首个样本；直方图取 _sum）
function parseProm(text){
 const out={};for(const line of text.split("\n")){
  if(!line||line[0]==="#")continue;
  const sp=line.lastIndexOf(" ");if(sp<0)continue;
  const name=line.slice(0,sp).trim(),v=parseFloat(line.slice(sp+1));
  if(Number.isNaN(v))continue;
  if(name.endsWith("_sum")&&!out[name])out[name]=v;
  else if(!(name in out))out[name]=v;
 }return out}
function pushAlarm(level,msg){
 const t=new Date().toLocaleTimeString();
 alarms.unshift({t,level,msg});alarms=alarms.slice(0,20);renderAlarms()}
function renderAlarms(){
 $("alarms").innerHTML=alarms.length?alarms.map(a=>
  `<div class="alarm ${a.level==='warn'?'warn':''}"><b>${a.t}</b> ${a.msg}</div>`).join("")
 :'<div class="alarm warn">（无）</div>'}
function row(k,v){return `<tr><td>${k}</td><td>${v}</td></tr>`}
async function jget(p){const r=await fetch(p);if(!r.ok)throw new Error(p+" HTTP "+r.status);return r.json()}
async function poll(){
 let health,inspect,ents,prom;
 try{[health,inspect,ents]=await Promise.all([jget("/health"),jget("/inspect"),jget("/entities")]);
  const mr=await fetch("/metrics");prom=parseProm(await mr.text());
  $("poll").textContent="ok · "+new Date().toLocaleTimeString();$("poll").className="sub";
 }catch(e){$("poll").textContent="fetch failed: "+e.message;$("poll").className="sub err";return}
 txt("role",health.role+" v"+health.version+" · component "+health.component_id);
 txt("uptime","up "+health.uptime_s+"s · "+(health.liveness?"liveness ✓":"liveness ✗"));
 txt("sessions",inspect.runtime.session_count);
 txt("entities",inspect.runtime.entity_count);
 $("entities-types").textContent=(inspect.runtime.entity_types||[]).join(", ")||" ";
 const td=prom["tick_duration_ms_sum"]||0,tc=prom["tick_duration_ms_count"]||0;
 txt("tick",tc>0?(td/tc).toFixed(2)+" ms":"-");
 $("tick-sub").textContent="count "+tc+" · slow "+(prom["slow_tick_count"]||0);
 const rss=prom["process_rss_bytes"];
 // 0 表示非 Linux/读失败（OpsInspector 注释契约）：显示为不可用而非伪值 "0 B"。
 $("rss").textContent=(rss===undefined||rss===0)?"n/a":fmtBytes(rss);
 const qo=prom["queue_backlog"]||0,qi=prom["inbound_queue_depth"]||0;
 txt("qout",qo);$("qin").textContent="inbound "+qi;
 const now=performance.now();
 // 越限告警只在上升沿触发：qo 持续 >1000 期间每 2s 轮询不得重复刷屏。
 // 首轮 poll（prev 为 null）队列已越限时也须告警——把 prev 缺失视为
 // 基线 0，首次越限即视为从"未越限"上升，不吞掉首屏告警。
 const prevQo=prev?(prev["queue_backlog"]||0):0;
 if(qo>1000&&prevQo<=1000)pushAlarm("warn","queue_backlog = "+qo+"（>1000）");
 if(prev){
  const dt=(now-prevAt)/1000;
  const dSent=((prom["transport_messages_sent_total"]||0)-(prev["transport_messages_sent_total"]||0))/dt;
  const dRecv=((prom["transport_messages_received_total"]||0)-(prev["transport_messages_received_total"]||0))/dt;
  txt("thr",(dSent+dRecv).toFixed(0));
  $("thr-sub").textContent="sent "+dSent.toFixed(0)+"/s · recv "+dRecv.toFixed(0)+"/s";
  // 告警推导：对比上次采样的增量与阈值
  const dSlow=(prom["slow_tick_count"]||0)-(prev["slow_tick_count"]||0);
  if(dSlow>0)pushAlarm("","slow_tick_count +"+dSlow+"（tick 超时）");
  const dBP=(prom["transport_backpressure_events_total"]||0)-(prev["transport_backpressure_events_total"]||0);
  if(dBP>0)pushAlarm("","backpressure events +"+dBP);
  const dEnt=(inspect.runtime.entity_count)-(prevEnt);
  if(dEnt<=-10)pushAlarm("warn","entity_count "+dEnt+"（骤降）");
 }
 prev=prom;prevEnt=inspect.runtime.entity_count;prevAt=now;
 let html=row("transport messages sent / received",
   (prom["transport_messages_sent_total"]||0)+" / "+(prom["transport_messages_received_total"]||0));
 html+=row("transport bytes sent / received",
   fmtBytes(prom["transport_bytes_sent_total"]||0)+" / "+fmtBytes(prom["transport_bytes_received_total"]||0));
 html+=row("backpressure",(prom["transport_backpressure"]||0)+"（active）· "+
   (prom["transport_backpressure_events_total"]||0)+"（total）");
 html+=row("login_pending_count",prom["login_pending_count"]===undefined?"n/a":prom["login_pending_count"]);
 for(const[k,v]of Object.entries(prom)){
  if(k.startsWith("login_")&&k!=="login_pending_count")html+=row(k,v);
 }
 $("detail").innerHTML=html;
 $("raw").textContent=Object.entries(prom).map(([k,v])=>k+" "+v).join("\n");
}
let prevEnt=null;
renderAlarms();poll();setInterval(poll,2000);
</script>
</body>
</html>)dashboard";
}

}  // namespace theseed::ops
