// 手机配网页面：扫描附近 Wi-Fi、提交密码、管理已保存的网络。
#pragma once

namespace esphome::passport_wifi {

inline constexpr char kPortalPage[] = R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AI Passport 语音 · 连接 Wi-Fi</title>
<style>
:root{--bg:#0b1219;--card:#121e2b;--line:#1e3348;--ink:#f0f6fa;--mute:#7fa0b8;--acc:#18bcf2}
*{box-sizing:border-box}body{margin:0;font:16px/1.5 -apple-system,system-ui,"PingFang SC",sans-serif;
background:radial-gradient(120% 60% at 50% 0,#0f2a3d 0,var(--bg) 60%);color:var(--ink);min-height:100vh}
main{max-width:420px;margin:0 auto;padding:28px 18px 40px}
h1{font-size:24px;margin:0 0 4px}.sub{color:var(--mute);margin:0 0 22px}
.card{background:var(--card);border:1px solid var(--line);border-radius:18px;padding:6px}
.net{display:flex;align-items:center;justify-content:space-between;width:100%;padding:14px;border:0;background:none;
color:var(--ink);font-size:16px;border-radius:12px;text-align:left}.net+.net{border-top:1px solid var(--line)}
.net.sel{background:#1a2c3d}.bars{color:var(--mute);font-size:13px;white-space:nowrap}
label{display:block;color:var(--mute);font-size:14px;margin:18px 4px 6px}
input{width:100%;padding:14px;border-radius:12px;border:1px solid var(--line);background:var(--card);color:var(--ink);font-size:16px}
button.go{width:100%;margin-top:20px;padding:15px;border:0;border-radius:14px;background:var(--acc);color:#04202c;font-size:17px;font-weight:700}
button.go:disabled{opacity:.5}.msg{margin-top:16px;color:var(--mute);text-align:center;min-height:24px}
.ok{color:#2ed598}.err{color:#ff5c5c}a{color:var(--acc)}
h2{font-size:16px;color:var(--mute);margin:30px 4px 8px;font-weight:600}.tip{color:var(--mute);font-size:13px;margin:10px 4px}
.del{border:0;background:#1e3348;color:var(--ink);border-radius:10px;padding:6px 12px;font-size:14px}
.tag{color:var(--mute);font-size:13px}
</style></head><body><main>
<h1>连接 Wi-Fi</h1><p class="sub">AI Passport 语音 · 添加家里的 Wi-Fi 或手机热点</p>
<div class="card" id="list"><div class="net">正在搜索附近的 Wi-Fi…</div></div>
<p style="text-align:right;margin:8px 4px 0"><a href="#" id="rescan">重新搜索</a></p>
<label for="ssid">Wi-Fi 名称（仅支持 2.4GHz）</label><input id="ssid" maxlength="32" autocomplete="off">
<label for="pass">密码</label><input id="pass" type="password" maxlength="64" autocomplete="off">
<button class="go" id="go">连接</button><div class="msg" id="msg"></div>
<h2>已保存的网络</h2><div class="card" id="saved"><div class="net">读取中…</div></div>
<p class="tip">最多保存 4 个，开机自动连接信号最强的一个。出门可添加手机热点：
iPhone 打开「个人热点 › 最大兼容性」，安卓选择 2.4GHz 频段。</p>
</main><script>
const $=id=>document.getElementById(id);
function bars(r){return r>-55?'信号强':r>-70?'信号中':'信号弱'}
async function scan(){$('list').innerHTML='<div class="net">正在搜索附近的 Wi-Fi…</div>';
try{const a=await(await fetch('/scan')).json();if(!a.length){$('list').innerHTML='<div class="net">没有找到，可手动输入名称</div>';return}
$('list').innerHTML='';a.forEach(n=>{const b=document.createElement('button');b.className='net';
b.innerHTML='<span></span><span class="bars">'+(n.l?'加密 · ':'')+bars(n.r)+'</span>';b.firstChild.textContent=n.s;
b.onclick=()=>{document.querySelectorAll('.net').forEach(x=>x.classList.remove('sel'));b.classList.add('sel');$('ssid').value=n.s;$('pass').focus()};
$('list').appendChild(b)})}catch(e){$('list').innerHTML='<div class="net">搜索失败，请重试</div>'}}
$('rescan').onclick=e=>{e.preventDefault();scan()};
async function saved(){try{const a=await(await fetch('/saved')).json();$('saved').innerHTML='';
if(!a.length){$('saved').innerHTML='<div class="net">还没有保存的网络</div>';return}
a.forEach(n=>{const d=document.createElement('div');d.className='net';
d.innerHTML='<span></span>'+(n.b?'<span class="tag">固件内置</span>':'<button class="del">删除</button>');
d.firstChild.textContent=n.s;if(!n.b)d.lastChild.onclick=async()=>{await fetch('/forget',{method:'POST',
headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'s='+encodeURIComponent(n.s)});saved()};$('saved').appendChild(d)})}
catch(e){$('saved').innerHTML='<div class="net">读取失败</div>'}}
async function poll(n){try{const s=await(await fetch('/status')).json();
if(s.st=='connected'){$('msg').className='msg ok';$('msg').textContent='连接成功！胸牌马上回到语音界面，可以关闭本页面。';saved();return}
if(s.st=='failed'){$('msg').className='msg err';$('msg').textContent='连接失败：请检查密码，或靠近路由器后重试。';$('go').disabled=false;return}}
catch(e){if(n>3){$('msg').className='msg';$('msg').textContent='手机与胸牌的连接已断开。请看胸牌屏幕：显示“连接成功”即完成。';return}}
if(n<40)setTimeout(()=>poll(n+1),1500)}
$('go').onclick=async()=>{const s=$('ssid').value.trim();if(!s){$('msg').className='msg err';$('msg').textContent='请先选择或输入 Wi-Fi 名称';return}
$('go').disabled=true;$('msg').className='msg';$('msg').textContent='正在连接…';
try{await fetch('/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
body:'s='+encodeURIComponent(s)+'&p='+encodeURIComponent($('pass').value)});}catch(e){}poll(0)};
scan();saved();
</script></body></html>)HTML";

}  // namespace esphome::passport_wifi
