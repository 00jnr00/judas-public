# Built-in tools: web and images

## web_search

`web_search {query, max_results?, category?}` searches through the local SearXNG (`[search] searxng`).

- `category`: `general` (default), `images` or `news`. It is passed to SearXNG as `categories=`. Any other value
  is refused with a message listing the three.
- `general` and `news` return numbered results with title, url, a snippet and (when the result has one) an
  `image:` thumbnail URL.
- `images` returns, per result, the title, `page:` (the page the picture is on) and `img_src:` (the picture
  itself). Results without an http(s) `img_src` are dropped.

## show_image

`show_image {source, caption?}` puts a real image in front of the **user** (unlike `view_image`, which only the
model sees).

- `source` is an `http://` or `https://` image URL, or a file path.
- URL: the server fetches it (5 s connect, 15 s read, 30 s total, at most 4 redirects, each checked to be
  http or https). The response must have an `image/*` content type (SVG is refused) and the bytes must really be
  a PNG, JPEG, GIF or WebP. The size cap is the smaller of 15 MB and `[attachments] max_bytes`.
- File path: resolved like `read_file`, then it must be inside `permissions.allow_paths` (or the working
  directory), not match `deny_paths`, and not be a secret (`secret_paths`, `.ssh`, `.gnupg`, Judas' own data).
  A symlink is judged by where it points. Anything else is refused.
- The bytes are stored as an attachment (`docs/ATTACHMENTS.md`). The result is a ready-to-paste line:

  ```
  ![caption](/api/attachments/0123456789abcdef)
  attachment id: 0123456789abcdef
  ```

  Because the server fetched the picture, the browser loads `/api/attachments/<id>` directly: no click-to-load
  placeholder and no request to the remote site from the browser.

The usual flow for "show me X": `web_search {query: "X", category: "images"}`, then
`show_image {source: <img_src>, caption: "X"}`, then paste the markdown line into the reply. The id also works as
the `image:` of a `cards` item.

## view_image and screenshot

Both load an image for the model to look at (`docs/ATTACHMENTS.md`); the user does not see the result.
