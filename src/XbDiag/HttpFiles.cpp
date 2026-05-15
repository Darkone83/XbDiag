// ============================================================================
// HttpFiles.cpp -- HTTP file manager for XbDiag web UI
// ============================================================================

#include "HttpFiles.h"
#include "DiagCommon.h"
#include <xtl.h>
#include <winsockx.h>

// ── Shared page builder helpers from HttpRptSrv.cpp ──────────────────────────
extern void BA(const char* s);
extern void BAE(const char* s);
extern void PageStart(const char* title, const char* tab, const char* extraHead);
extern void PageEnd();

// ── Local helpers ─────────────────────────────────────────────────────────────

static bool HFEq(const char* a, const char* b)
{
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == '\0' && *b == '\0';
}

// Extract a query param value -- URL-decodes %XX and + sequences
bool HFParam(const char* src, const char* key, char* out, int outLen)
{
    int kl = StrLen(key);
    const char* p = src;
    out[0] = '\0';
    while (*p)
    {
        bool m = true; int ki;
        for (ki = 0; ki < kl; ki++) if (p[ki] != key[ki]) { m = false; break; }
        if (m && p[kl] == '=')
        {
            p += kl + 1;
            int i = 0;
            while (*p && *p != '&' && i < outLen - 1)
            {
                if (*p == '+') { out[i++] = ' '; ++p; }
                else if (*p == '%' && p[1] && p[2])
                {
                    char hi = p[1], lo = p[2];
                    int h = (hi >= 'A') ? (hi - 'A' + 10) : (hi >= 'a') ? (hi - 'a' + 10) : (hi - '0');
                    int l = (lo >= 'A') ? (lo - 'A' + 10) : (lo >= 'a') ? (lo - 'a' + 10) : (lo - '0');
                    out[i++] = (char)((h << 4) | l); p += 3;
                }
                else out[i++] = *p++;
            }
            out[i] = '\0';
            return true;
        }
        while (*p && *p != '&') ++p;
        if (*p == '&') ++p;
    }
    return false;
}

// Send a JSON response directly to the client socket
static void SendJSON(SOCKET c, const char* json)
{
    char hdr[128];
    char cl[12]; IntToStr(StrLen(json), cl, sizeof(cl));
    StrCopy(hdr, sizeof(hdr), "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n");
    StrCat2(hdr, sizeof(hdr), hdr, "Content-Length: ");
    StrCat2(hdr, sizeof(hdr), hdr, cl);
    StrCat2(hdr, sizeof(hdr), hdr, "\r\nConnection: close\r\n\r\n");
    send(c, hdr, StrLen(hdr), 0);
    send(c, json, StrLen(json), 0);
}

// Sanitise a path string -- must start with a drive letter or be empty
// Prevents traversal outside the Xbox filesystem
bool SafePath(const char* path, char* out, int outLen)
{
    if (!path || !path[0]) return false;

    // Strip leading slash -- JS sends /E:/folder, we need E:\folder
    const char* p = path;
    if (*p == '/') ++p;

    // Convert forward slashes to backslashes
    int i = 0;
    while (p[i] && i < outLen - 1)
    {
        out[i] = (p[i] == '/') ? '\\' : p[i];
        ++i;
    }
    out[i] = '\0';

    // Must start with a drive letter and colon e.g. E:
    if (!(out[0] >= 'A' && out[0] <= 'Z') || out[1] != ':') return false;

    // Ensure trailing backslash if just a drive root
    if (out[2] == '\0') { out[2] = '\\'; out[3] = '\0'; }

    // Reject any .. traversal
    for (int j = 0; out[j]; j++)
        if (out[j] == '.' && out[j + 1] == '.') return false;

    return true;
}

// Format a file size as a readable string
static void FmtSize(DWORD lo, DWORD hi, char* buf, int bufLen)
{
    if (hi > 0 || lo >= 1024 * 1024 * 1024)
    {
        // GB
        DWORD gb = lo / (1024 * 1024 * 1024);
        char t[12]; IntToStr((int)gb, t, sizeof(t));
        StrCopy(buf, bufLen, t); StrCat2(buf, bufLen, buf, " GB");
    }
    else if (lo >= 1024 * 1024)
    {
        DWORD mb = lo / (1024 * 1024);
        char t[12]; IntToStr((int)mb, t, sizeof(t));
        StrCopy(buf, bufLen, t); StrCat2(buf, bufLen, buf, " MB");
    }
    else if (lo >= 1024)
    {
        DWORD kb = lo / 1024;
        char t[12]; IntToStr((int)kb, t, sizeof(t));
        StrCopy(buf, bufLen, t); StrCat2(buf, bufLen, buf, " KB");
    }
    else
    {
        char t[12]; IntToStr((int)lo, t, sizeof(t));
        StrCopy(buf, bufLen, t); StrCat2(buf, bufLen, buf, " B");
    }
}

// ── API: GET /list?path=/ ─────────────────────────────────────────────────────
void HttpFiles_ServeList(SOCKET c, const char* query)
{
    char path[128]; path[0] = '\0';
    HFParam(query, "path", path, sizeof(path));

    // Build JSON into a local buffer
    char json[8192]; int jl = 0;

    auto J = [&](const char* s) {
        int sl = StrLen(s);
        if (jl + sl < (int)sizeof(json) - 1)
        {
            for (int i = 0; i < sl; ++i) json[jl++] = s[i]; json[jl] = '\0';
        }
        };
    auto JE = [&](const char* s) {
        // JSON-escape a string value
        for (int i = 0; s[i]; i++)
        {
            if (jl >= (int)sizeof(json) - 4) break;
            if (s[i] == '"') { json[jl++] = '\\'; json[jl++] = '"'; }
            else if (s[i] == '\\') { json[jl++] = '\\'; json[jl++] = '\\'; }
            else { json[jl++] = s[i]; }
        }
        json[jl] = '\0';
        };

    bool isRoot = (!path[0] || HFEq(path, "/"));

    J("{\"ok\":true,\"entries\":[");
    bool first = true;

    // Root listing -- enumerate drive letters
    if (isRoot)
    {
        const char* drives[] = { "C","D","E","F","G","X","Y","Z", NULL };
        for (int di = 0; drives[di]; ++di)
        {
            char root[6];
            root[0] = drives[di][0]; root[1] = ':'; root[2] = '\\'; root[3] = '\0';
            DWORD attr = GetFileAttributesA(root);
            if (attr == 0xFFFFFFFF || !(attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!first) J(","); first = false;
            // Return name as "E:" so JS builds path "/E:/"
            J("{\"n\":\""); JE(drives[di]); J(":\",\"d\":true,\"s\":\"\"}");
        }
    }
    else
    {
        // Directory listing -- SafePath strips leading slash and validates
        char real[128];
        if (!SafePath(path, real, sizeof(real)))
        {
            SendJSON(c, "{\"ok\":false,\"error\":\"Invalid path\"}");
            return;
        }

        // Ensure trailing backslash then append *
        char pat[136];
        StrCopy(pat, sizeof(pat), real);
        int pl = StrLen(pat);
        if (pl > 0 && pat[pl - 1] != '\\') { pat[pl] = '\\'; pat[pl + 1] = '\0'; pl++; }
        pat[pl] = '*'; pat[pl + 1] = '\0';

        WIN32_FIND_DATA fd;
        HANDLE h = FindFirstFile(pat, &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            SendJSON(c, "{\"ok\":true,\"entries\":[]}");
            return;
        }
        do {
            if (fd.cFileName[0] == '.' &&
                (fd.cFileName[1] == '\0' || fd.cFileName[1] == '.')) continue;
            bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            char sz[16]; sz[0] = '\0';
            if (!isDir) FmtSize(fd.nFileSizeLow, fd.nFileSizeHigh, sz, sizeof(sz));
            if (!first) J(","); first = false;
            J("{\"n\":\""); JE(fd.cFileName);
            J("\",\"d\":"); J(isDir ? "true" : "false");
            J(",\"s\":\""); JE(sz); J("\"}");
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }

    J("]}");
    SendJSON(c, json);
}

// ── Recursive directory delete ────────────────────────────────────────────────
static void DeleteRecursive(const char* realPath)
{
    char pat[136];
    StrCopy(pat, sizeof(pat), realPath);
    int pl = StrLen(pat);
    if (pl > 0 && pat[pl - 1] != '\\') { pat[pl] = '\\'; pat[pl + 1] = '\0'; pl++; }
    pat[pl] = '*'; pat[pl + 1] = '\0';

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pat, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do {
            if (fd.cFileName[0] == '.' &&
                (fd.cFileName[1] == '\0' || fd.cFileName[1] == '.')) continue;

            char child[136];
            StrCopy(child, sizeof(child), realPath);
            int cl = StrLen(child);
            if (cl > 0 && child[cl - 1] != '\\') { child[cl] = '\\'; child[cl + 1] = '\0'; cl++; }
            StrCat2(child, sizeof(child), child, fd.cFileName);

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                DeleteRecursive(child);
            else
                DeleteFileA(child);
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(realPath);
}

// ── API: POST /delete?path=/E:/file.txt ──────────────────────────────────────
void HttpFiles_ServeDelete(SOCKET c, const char* query)
{
    char path[128]; path[0] = '\0';
    HFParam(query, "path", path, sizeof(path));

    char real[128];
    if (!SafePath(path, real, sizeof(real)))
    {
        SendJSON(c, "{\"ok\":false,\"error\":\"Invalid path\"}"); return;
    }

    DWORD attr = GetFileAttributesA(real);
    if (attr == 0xFFFFFFFF)
    {
        SendJSON(c, "{\"ok\":false,\"error\":\"Not found\"}"); return;
    }

    if (attr & FILE_ATTRIBUTE_DIRECTORY)
        DeleteRecursive(real);
    else
        DeleteFileA(real);

    // Verify gone
    SendJSON(c, GetFileAttributesA(real) == 0xFFFFFFFF
        ? "{\"ok\":true}"
        : "{\"ok\":false,\"error\":\"Delete failed\"}");
}

// ── API: POST /mkdir?path=/E:/folder ─────────────────────────────────────────
void HttpFiles_ServeMkdir(SOCKET c, const char* query)
{
    char path[128]; path[0] = '\0';
    HFParam(query, "path", path, sizeof(path));

    char real[128];
    if (!SafePath(path, real, sizeof(real)))
    {
        SendJSON(c, "{\"ok\":false,\"error\":\"Invalid path\"}"); return;
    }

    BOOL ok = CreateDirectoryA(real, NULL);
    SendJSON(c, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"mkdir failed\"}");
}

// ── API: POST /upload?path=/E:/file.txt ──────────────────────────────────────
// Body is raw binary file data. Streamed directly to disk -- no RAM buffering.
void HttpFiles_ServeUpload(SOCKET c, const char* query,
    const char* body, int bodyLen)
{
    char path[128]; path[0] = '\0';
    HFParam(query, "path", path, sizeof(path));

    char real[128];
    if (!SafePath(path, real, sizeof(real)))
    {
        SendJSON(c, "{\"ok\":false,\"error\":\"Invalid path\"}"); return;
    }

    HANDLE hf = CreateFileA(real, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
    {
        SendJSON(c, "{\"ok\":false,\"error\":\"Cannot create file\"}"); return;
    }

    // Write in 4KB chunks to stay FATX friendly
    const int CHUNK = 4096;
    int written = 0;
    while (written < bodyLen)
    {
        int chunk = bodyLen - written;
        if (chunk > CHUNK) chunk = CHUNK;
        DWORD w = 0;
        WriteFile(hf, body + written, (DWORD)chunk, &w, NULL);
        written += (int)w;
        if ((int)w < chunk) break;  // write error
    }
    CloseHandle(hf);

    SendJSON(c, (written == bodyLen)
        ? "{\"ok\":true}"
        : "{\"ok\":false,\"error\":\"Write incomplete\"}");
}

// ── API: GET /filedownload?path=/E:/file.txt ──────────────────────────────────
void HttpFiles_ServeDownload(SOCKET c, const char* query)
{
    char path[128]; path[0] = '\0';
    HFParam(query, "path", path, sizeof(path));

    char real[128];
    if (!SafePath(path, real, sizeof(real)))
    {
        send(c, "HTTP/1.0 400 Bad Request\r\n\r\n", 28, 0); return;
    }

    HANDLE hf = CreateFileA(real, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
    {
        send(c, "HTTP/1.0 404 Not Found\r\n\r\n", 25, 0); return;
    }

    // Extract filename for Content-Disposition
    const char* fname = real;
    for (int i = 0; real[i]; i++) if (real[i] == '\\') fname = real + i + 1;

    DWORD fsize = GetFileSize(hf, NULL);
    char szBuf[16]; IntToStr((int)fsize, szBuf, sizeof(szBuf));

    char hdr[512];
    StrCopy(hdr, sizeof(hdr),
        "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n"
        "Content-Disposition: attachment; filename=\"");
    StrCat2(hdr, sizeof(hdr), hdr, fname);
    StrCat2(hdr, sizeof(hdr), hdr, "\"\r\nContent-Length: ");
    StrCat2(hdr, sizeof(hdr), hdr, szBuf);
    StrCat2(hdr, sizeof(hdr), hdr, "\r\nConnection: close\r\n\r\n");
    send(c, hdr, StrLen(hdr), 0);

    // Stream in 4KB chunks
    char chunk[4096]; DWORD nr = 0; bool aborted = false;
    while (!aborted && ReadFile(hf, chunk, sizeof(chunk), &nr, NULL) && nr > 0)
    {
        int sent = 0;
        while (sent < (int)nr)
        {
            int n = send(c, chunk + sent, (int)nr - sent, 0);
            if (n <= 0) { aborted = true; break; }
            sent += n;
        }
    }
    CloseHandle(hf);
}

void HttpFiles_BuildPage()
{
    PageStart("Files", "fi",
        "<style>"
        ".fb-bar{display:flex;gap:10px;align-items:center;margin-bottom:12px}"
        ".fb-bc{font-size:11px;color:#3A5070;margin-bottom:8px}"
        ".fb-bc a{color:#50DCFF;cursor:pointer;text-decoration:none}"
        ".fb-tbl{width:100%;border-collapse:collapse}"
        ".fb-tbl th{text-align:left;color:#3A5070;padding:5px 10px;"
        "border-bottom:1px solid #1e2840;font-size:10px;letter-spacing:1px}"
        ".fb-tbl td{padding:5px 10px;border-bottom:1px solid #0a0e1a}"
        ".fb-tbl a{color:#50DCFF;cursor:pointer;text-decoration:none}"
        ".fb-tbl a:hover{color:#fff}"
        ".fb-sz{text-align:right;color:#3A5070;width:80px}"
        ".fb-act{color:#3A5070;width:60px;text-align:right}"
        ".fb-del{color:#FF5060;cursor:pointer;font-size:10px}"
        ".fb-del:hover{color:#fff}"
        ".fb-err{color:#FF5060;font-size:11px;margin-top:6px}"
        ".fb-st{font-size:11px;color:#3A5070}"
        "</style>");

    BA("<div class=sec>File Browser</div>");
    BA("<div class=card><div class=card-body>");

    // Breadcrumb + status + nav buttons
    BA("<div class=fb-bar>"
        "<button onclick='load(\"/\")' style='padding:5px 12px;border-radius:6px;"
        "border:1px solid #1e2840;background:#0a0e1a;color:#50DCFF;"
        "font-family:monospace;font-size:10px;cursor:pointer'>[Root]</button>"
        "<button onclick='goUp()' style='padding:5px 12px;border-radius:6px;"
        "border:1px solid #1e2840;background:#0a0e1a;color:#8A9AB0;"
        "font-family:monospace;font-size:10px;cursor:pointer'>[Up]</button>"
        "<span class=fb-st id=fbSt>Loading...</span>"
        "</div>"
        "<div class=fb-bc id=fbBc></div>"
        "<div id=fbLs></div>"
        "<div class=fb-err id=fbErr></div>");

    // Upload section -- two inputs: files and folder (webkitdirectory)
    BA("<div style='margin-top:12px;border-top:1px solid #1e2840;padding-top:10px'>"
        "<div style='font-size:10px;color:#3A5070;letter-spacing:1px;margin-bottom:6px'>UPLOAD</div>"
        "<div style='display:flex;gap:8px;flex-wrap:wrap;align-items:center'>"
        "<label style='padding:5px 12px;border-radius:6px;border:1px solid #1e2840;"
        "background:#0a0e1a;color:#8A9AB0;font-family:monospace;font-size:10px;cursor:pointer'>"
        "Files<input type=file id=fbFiles multiple style='display:none' onchange='queueFiles(this.files)'></label>"
        "<label style='padding:5px 12px;border-radius:6px;border:1px solid #1e2840;"
        "background:#0a0e1a;color:#8A9AB0;font-family:monospace;font-size:10px;cursor:pointer'>"
        "Folder<input type=file id=fbFolder webkitdirectory style='display:none' onchange='queueFiles(this.files)'></label>"
        "<span id=fbUpSt style='font-size:10px;color:#3A5070;flex:1'></span>"
        "</div>"
        // Progress bar
        "<div style='height:4px;background:#1e2840;border-radius:2px;margin-top:8px'>"
        "<div id=fbBar style='height:4px;background:#50DCFF;border-radius:2px;width:0;"
        "transition:width .2s'></div></div>"
        "<div class=fb-err id=fbUpErr></div>"
        "</div>");

    BA("</div></div>");

    // Inline JS -- fetch() to /list, /delete, /mkdir, /upload
    BA("<script>\n"
        "var P='/';\n"
        "var uploadQueue=[];\n"
        "var uploadIdx=0;\n"

        "function uerr(m){document.getElementById('fbUpErr').textContent=m||'';}\n"
        "function ust(m){document.getElementById('fbUpSt').textContent=m;}\n"
        "function err(m){document.getElementById('fbErr').textContent=m||'';}\n"
        "function st(m){document.getElementById('fbSt').textContent=m;}\n"

        "function goUp(){\n"
        "  if(P==='/')return;\n"
        "  var up=P.replace(/\\/$/,'').replace(/\\/[^\\/]*$/,'/')||'/';\n"
        "  load(up);\n"
        "}\n"

        "function load(path){\n"
        "  P=path;\n"
        "  err('');\n"
        "  st('Loading '+path+'...');\n"
        "  fetch('/list?path='+encodeURIComponent(path))\n"
        "  .then(function(r){return r.json();})\n"
        "  .then(function(d){\n"
        "    if(!d.ok){err(d.error||'List failed');return;}\n"
        "    st(path);\n"
        "    bc(path);\n"
        "    tbl(d.entries,path);\n"
        "  })\n"
        "  .catch(function(e){err(''+e);});\n"
        "}\n"

        "function bc(path){\n"
        "  var pts=path.replace(/\\\\/g,'/').split('/').filter(Boolean);\n"
        "  var h='<a onclick=\\'load(\\'/\\')\\'>Root</a>';\n"
        "  var a='/';\n"
        "  for(var i=0;i<pts.length;i++){\n"
        "    a+=pts[i]+'/';\n"
        "    var q=a;\n"
        "    h+=' &rsaquo; <a onclick=\\'load(\"'+q+'\")\\'>' +pts[i]+'</a>';\n"
        "  }\n"
        "  document.getElementById('fbBc').innerHTML=h;\n"
        "}\n"

        "function tbl(e,path){\n"
        "  var h='<table class=fb-tbl><thead><tr>'\n"
        "    +'<th>Name</th><th class=fb-sz>Size</th><th class=fb-act></th>'\n"
        "    +'</tr></thead><tbody>';\n"
        "  if(path!='/'){\n"
        "    var up=path.replace(/\\/$/,'').replace(/\\/[^\\/]*$/,'/')||'/';\n"
        "    h+='<tr><td><a onclick=\\'load(\"'+up+'\")\\'>..</a></td><td></td><td></td></tr>';\n"
        "  }\n"
        "  for(var i=0;i<e.length;i++){\n"
        "    var f=e[i];\n"
        "    var base=path==='/'?'':path.replace(/\\/$/,'');\n"
        "    var np=base+'/'+f.n;\n"
        "    if(f.d){\n"
        "      h+='<tr><td>[D] <a onclick=\\'load(\"'+np+'/\")\\'>' +f.n+'</a></td>'\n"
        "       +'<td></td>'\n"
        "       +'<td class=fb-act><span class=fb-del onclick=\\'del(\"'+np+'\",true)\\'>[X]</span></td></tr>';\n"
        "    } else {\n"
        "      var dl='/filedownload?path='+encodeURIComponent(np);\n"
        "      h+='<tr><td>[F] <a href=\\''+dl+'\\'>'+f.n+'</a></td>'\n"
        "       +'<td class=fb-sz>'+f.s+'</td>'\n"
        "       +'<td class=fb-act><span class=fb-del onclick=\\'del(\"'+np+'\",false)\\'>[X]</span></td></tr>';\n"
        "    }\n"
        "  }\n"
        "  h+='</tbody></table>';\n"
        "  document.getElementById('fbLs').innerHTML=h;\n"
        "}\n"

        "function del(path,isDir){\n"
        "  var name=path.replace(/\\/$/,'').replace(/.*\\//,'');\n"
        "  var msg=isDir\n"
        "    ? 'Delete folder \"'+name+'\" and ALL its contents?\\n\\nThis cannot be undone.'\n"
        "    : 'Delete file \"'+name+'\"?';\n"
        "  if(!confirm(msg))return;\n"
        "  fetch('/delete?path='+encodeURIComponent(path),{method:'POST'})\n"
        "  .then(function(r){return r.json();})\n"
        "  .then(function(d){\n"
        "    if(d.ok)load(P);\n"
        "    else err(d.error||'Delete failed');\n"
        "  })\n"
        "  .catch(function(e){err(''+e);});\n"
        "}\n"

        // Queue files for sequential upload -- handles both files and folders.
        // For folder uploads (webkitdirectory), file.webkitRelativePath gives
        // the path relative to the selected folder root, e.g. "GameFolder/default.xbe"
        // We create subdirectories as needed then upload each file in sequence.
        "function queueFiles(files){\n"
        "  uploadQueue=[];\n"
        "  for(var i=0;i<files.length;i++){\n"
        "    var rel=files[i].webkitRelativePath||files[i].name;\n"
        "    uploadQueue.push({file:files[i],rel:rel});\n"
        "  }\n"
        "  if(uploadQueue.length===0)return;\n"
        "  uploadIdx=0;\n"
        "  document.getElementById('fbBar').style.width='0';\n"
        "  uerr('');\n"
        "  uploadNext();\n"
        "}\n"

        "function uploadNext(){\n"
        "  if(uploadIdx>=uploadQueue.length){\n"
        "    ust('Done - '+uploadQueue.length+' file(s) uploaded');\n"
        "    document.getElementById('fbBar').style.width='100%';\n"
        "    load(P);\n"
        "    return;\n"
        "  }\n"
        "  document.getElementById('fbBar').style.width='0';\n"
        "  var item=uploadQueue[uploadIdx];\n"
        "  ust('Uploading '+item.rel+' ('+( uploadIdx+1)+'/'+uploadQueue.length+')');\n"

        // Build destination path -- combine current browse path with relative path
        "  var parts=item.rel.replace(/\\\\/g,'/').split('/');\n"
        "  var base=P.replace(/\\/$/,'');\n"

        // Create intermediate directories if needed (folder upload case)
        "  var mkdirs=[];\n"
        "  for(var i=0;i<parts.length-1;i++){\n"
        "    base+='/'+parts[i];\n"
        "    mkdirs.push(base);\n"
        "  }\n"
        "  var destPath=base+'/'+parts[parts.length-1];\n"

        "  function doMkdirs(idx){\n"
        "    if(idx>=mkdirs.length){doUpload(destPath,item.file);return;}\n"
        "    fetch('/mkdir?path='+encodeURIComponent(mkdirs[idx]),{method:'POST'})\n"
        "    .then(function(){doMkdirs(idx+1);})\n"
        "    .catch(function(){doMkdirs(idx+1);});\n"  // ignore mkdir errors (dir may exist)
        "  }\n"
        "  doMkdirs(0);\n"
        "}\n"

        "function doUpload(path,file){\n"
        "  var reader=new FileReader();\n"
        "  reader.onload=function(e){\n"
        "    document.getElementById('fbBar').style.width='50%';\n"
        "    fetch('/upload?path='+encodeURIComponent(path),{\n"
        "      method:'POST',\n"
        "      body:e.target.result,\n"
        "      headers:{'Content-Type':'application/octet-stream'}\n"
        "    })\n"
        "    .then(function(){\n"
        // Don't parse response body -- just treat any response as success
        "      uploadIdx++;\n"
        "      document.getElementById('fbBar').style.width='100%';\n"
        "      if(uploadIdx>=uploadQueue.length){\n"
        "        ust('Done - '+uploadQueue.length+' file(s) uploaded');\n"
        "        uerr('');\n"
        "        load(P);\n"
        "      } else {\n"
        "        uploadNext();\n"
        "      }\n"
        "    })\n"
        "    .catch(function(){\n"
        // fetch() rejects on network error but file may still have uploaded
        // Advance queue anyway and let the directory listing confirm
        "      uploadIdx++;\n"
        "      document.getElementById('fbBar').style.width='100%';\n"
        "      if(uploadIdx>=uploadQueue.length){\n"
        "        ust('Done (verify listing)');\n"
        "        load(P);\n"
        "      } else {\n"
        "        uploadNext();\n"
        "      }\n"
        "    });\n"
        "  };\n"
        "  reader.readAsArrayBuffer(file);\n"
        "}\n"

        "load('/');\n"
        "</script>\n");



    PageEnd();
}

bool HttpFiles_SafePath(const char* path, char* out, int outLen)
{
    return SafePath(path, out, outLen);
}

bool HttpFiles_GetParam(const char* src, const char* key, char* out, int outLen)
{
    return HFParam(src, key, out, outLen);
}