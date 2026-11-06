/* Markdown viewer for markdown artifacts. Reads the file named by <body data-doc="...">, escapes everything, and
   renders headings, lists, code, quotes, tables, links and images. No network: images must be files of the artifact. */
(function () {
  "use strict";
  var dark = window.matchMedia && matchMedia("(prefers-color-scheme: dark)").matches;
  var css = "html{background:" + (dark ? "#17181b" : "#fff") + "}body{margin:0;padding:28px 22px 60px;max-width:46rem;margin-inline:auto;" +
    "font:16px/1.65 system-ui,-apple-system,Segoe UI,sans-serif;color:" + (dark ? "#d8dae0" : "#23262d") + "}" +
    "h1,h2,h3,h4{line-height:1.25;margin:1.6em 0 .5em}h1{font-size:1.9rem}h2{font-size:1.45rem}h3{font-size:1.15rem}" +
    "p,ul,ol,blockquote,pre,table{margin:.8em 0}code{font:.9em ui-monospace,monospace;background:" + (dark ? "#26282e" : "#f0f1f4") + ";padding:.1em .35em;border-radius:5px}" +
    "pre{background:" + (dark ? "#202227" : "#f4f5f7") + ";padding:12px 14px;border-radius:10px;overflow:auto}pre code{background:none;padding:0}" +
    "blockquote{margin-left:0;padding-left:14px;border-left:3px solid " + (dark ? "#3a3d45" : "#d5d8de") + ";color:" + (dark ? "#a9adb8" : "#555b66") + "}" +
    "table{border-collapse:collapse}th,td{border:1px solid " + (dark ? "#34373e" : "#d5d8de") + ";padding:5px 10px}img{max-width:100%}hr{border:0;border-top:1px solid " + (dark ? "#34373e" : "#d5d8de") + "}" +
    "a{color:" + (dark ? "#8ab4f8" : "#1a5fb4") + "}";
  var st = document.createElement("style");
  st.textContent = css;
  document.head.appendChild(st);

  function esc(s) { return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;"); }
  function inline(s) {
    var codes = [];
    s = s.replace(/`([^`]+)`/g, function (_, c) { codes.push(c); return "\u0000" + (codes.length - 1) + "\u0000"; });
    s = esc(s);
    s = s.replace(/!\[([^\]]*)\]\(([^)\s]+)\)/g, function (_, alt, src) {
      return /^[A-Za-z0-9._\/-]+$/.test(src) && src.indexOf("..") < 0 ? '<img alt="' + alt + '" src="' + src + '">' : alt;
    });
    s = s.replace(/\[([^\]]+)\]\((https?:\/\/[^)\s]+|#[^)\s]*)\)/g, '<a href="$2" rel="noopener noreferrer">$1</a>');
    s = s.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>").replace(/(^|[\s(])\*([^*\n]+)\*(?=[\s).,;:!?]|$)/g, "$1<em>$2</em>");
    s = s.replace(/\u0000(\d+)\u0000/g, function (_, i) { return "<code>" + esc(codes[+i]) + "</code>"; });
    return s;
  }
  function render(md) {
    var lines = md.replace(/\r\n?/g, "\n").split("\n"), out = [], i = 0;
    function para(buf) { if (buf.length) out.push("<p>" + inline(buf.join(" ")) + "</p>"); buf.length = 0; }
    var buf = [];
    while (i < lines.length) {
      var l = lines[i], m;
      if ((m = /^```(.*)$/.exec(l))) {
        para(buf);
        var code = [];
        i++;
        while (i < lines.length && !/^```/.test(lines[i])) code.push(lines[i++]);
        i++;
        out.push("<pre><code>" + esc(code.join("\n")) + "</code></pre>");
      } else if ((m = /^(#{1,4})\s+(.*)$/.exec(l))) {
        para(buf); out.push("<h" + m[1].length + ">" + inline(m[2]) + "</h" + m[1].length + ">"); i++;
      } else if (/^\s*([-*_])(\s*\1){2,}\s*$/.test(l)) {
        para(buf); out.push("<hr>"); i++;
      } else if (/^>\s?/.test(l)) {
        para(buf);
        var q = [];
        while (i < lines.length && /^>\s?/.test(lines[i])) q.push(lines[i++].replace(/^>\s?/, ""));
        out.push("<blockquote>" + inline(q.join(" ")) + "</blockquote>");
      } else if (/^\s*([-*+]|\d+\.)\s+/.test(l)) {
        para(buf);
        var ordered = /^\s*\d+\./.test(l), items = [];
        while (i < lines.length && /^\s*([-*+]|\d+\.)\s+/.test(lines[i])) items.push(inline(lines[i++].replace(/^\s*([-*+]|\d+\.)\s+/, "")));
        out.push((ordered ? "<ol>" : "<ul>") + items.map(function (t) { return "<li>" + t + "</li>"; }).join("") + (ordered ? "</ol>" : "</ul>"));
      } else if (/^\|.*\|\s*$/.test(l) && i + 1 < lines.length && /^\|?\s*:?-{2,}/.test(lines[i + 1])) {
        para(buf);
        var rows = [];
        while (i < lines.length && /^\|.*\|\s*$/.test(lines[i])) rows.push(lines[i++]);
        var cells = function (r) { return r.replace(/^\||\|\s*$/g, "").split("|").map(function (c) { return inline(c.trim()); }); };
        var html = "<table><thead><tr>" + cells(rows[0]).map(function (c) { return "<th>" + c + "</th>"; }).join("") + "</tr></thead><tbody>";
        rows.slice(2).forEach(function (r) { html += "<tr>" + cells(r).map(function (c) { return "<td>" + c + "</td>"; }).join("") + "</tr>"; });
        out.push(html + "</tbody></table>");
      } else if (/^\s*$/.test(l)) {
        para(buf); i++;
      } else {
        buf.push(l.trim()); i++;
      }
    }
    para(buf);
    return out.join("\n");
  }
  var doc = document.body.getAttribute("data-doc");
  fetch(doc).then(function (r) { if (!r.ok) throw new Error("HTTP " + r.status); return r.text(); }).then(function (t) {
    var div = document.createElement("div");
    div.innerHTML = render(t);
    document.body.appendChild(div);
    document.documentElement.setAttribute("data-ready", "1");
  }, function (e) {
    document.body.textContent = "Could not load " + doc + ": " + e.message;
    document.documentElement.setAttribute("data-ready", "error");
  });
})();
