# Third-party notices

`literouter` is distributed under the Apache-2.0 License (see `LICENSE`). It links
the third-party components below. This file is the attribution notice for binaries
built from this repository: every entry is a package the build resolves through
the `mcpp.lock` files, with the license terms that have to travel with the binary.

| | member |
|---|---|
| ✔ | linked into that member's binary |
| · | not part of that member |

Every component is linked statically. This table is short because `literouter`
has exactly two members (`core` and `cli`) and the web console below; the GUI
member that once pulled in GLFW / FreeType / libpng / yyjson / the X.Org stack
has been removed, and so have its entries.

## Components

| Component | Version | License (SPDX) | core | cli |
|---|---|---|:---:|:---:|
| `compat.httplib` | 0.53.1 | MIT | ✔ | ✔ |
| `nlohmann.json` | 3.12.0 | MIT | ✔ | ✔ |
| `compat.CLI11` | 2.7.2 | BSD-3-Clause | · | ✔ |
| `compat.openssl` | 3.5.1 | Apache-2.0 | ✔ | ✔ |
| `compat.zlib` | 1.3.2 | Zlib | ✔ | ✔ |

### Web Console Frontend Components (Embedded)

The built-in web console is compiled into `literouter.core` via C++23 `#embed` and therefore bundled into the `cli` binary.

| Component | Version | License (SPDX) | core | cli |
|---|---|---|:---:|:---:|
| `react` / `react-dom` | 19.2.0 | MIT | ✔ | ✔ |
| `@radix-ui/react-dialog` | 1.1.15 | MIT | ✔ | ✔ |
| `@radix-ui/react-label` | 2.1.8 | MIT | ✔ | ✔ |
| `@radix-ui/react-select` | 2.2.6 | MIT | ✔ | ✔ |
| `@radix-ui/react-separator` | 1.1.8 | MIT | ✔ | ✔ |
| `@radix-ui/react-slot` | 1.2.4 | MIT | ✔ | ✔ |
| `@radix-ui/react-switch` | 1.2.6 | MIT | ✔ | ✔ |
| `@radix-ui/react-tooltip` | 1.2.8 | MIT | ✔ | ✔ |
| `lucide-react` | 0.545.0 | ISC | ✔ | ✔ |
| `clsx` | 2.1.1 | MIT | ✔ | ✔ |
| `tailwind-merge` | 3.3.1 | MIT | ✔ | ✔ |
| `class-variance-authority` | 0.7.1 | Apache-2.0 | ✔ | ✔ |
| `sonner` | 2.0.7 | MIT | ✔ | ✔ |
| `shadcn/ui` (components) | source copy | MIT | ✔ | ✔ |

Development-only tooling (build time, not embedded in binary):
- `vite` 7.1.9 (MIT)
- `tailwindcss` 4.1.14 (MIT)
- `tw-animate-css` 1.4.0 (MIT)
- `typescript` 5.9.3 (Apache-2.0)

## License texts

### MIT

Applies to `compat.httplib`, `nlohmann.json`, `react`, `react-dom`, `@radix-ui/*`,
`clsx`, `tailwind-merge`, `sonner` and `shadcn/ui` (each carries
`SPDX-License-Identifier: MIT`). Copyright lines:

```
cpp-httplib (compat.httplib)  Copyright (c) 2017 yhirose
nlohmann/json                 Copyright (c) 2013-2025 Niels Lohmann
react / react-dom             Copyright (c) Meta Platforms, Inc. and affiliates.
radix-ui primitives           Copyright (c) 2022 WorkOS
clsx                          Copyright (c) Luke Edwards <luke.edwards05@gmail.com> (lukeed.com)
tailwind-merge                Copyright (c) 2021 Dany Castillo
sonner                        Copyright (c) 2023 Emil Kowalski
shadcn/ui                     Copyright (c) 2023 shadcn
```

```text
The MIT License (MIT)

Copyright (c) 2017 yhirose

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### BSD-3-Clause

Applies to `compat.CLI11`.

```text
CLI11 2.7.2 Copyright (c) 2017-2026 University of Cincinnati, developed by Henry
Schreiner under NSF AWARD 1414736. All rights reserved.

Redistribution and use in source and binary forms of CLI11, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### Zlib

Applies to `compat.zlib` (the zlib library):

```text
Copyright notice:

 (C) 1995-2026 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu
```
`compat.zlib` is also the library cpp-httplib's decompression support links
against, which is why the member table above lists it for `core`.
### Apache-2.0

Applies to `compat.openssl` (OpenSSL) and `class-variance-authority`.
The full license text is the one in `LICENSE`, which this file does not duplicate.
Required attribution:

```text
OpenSSL — Copyright 1998-2025 The OpenSSL Project Authors. All Rights Reserved.
          This product includes software developed by the OpenSSL Project
          for use in the OpenSSL Toolkit (https://www.openssl.org/).
class-variance-authority — Copyright (c) 2022 Joe Bell
```

OpenSSL 3.x is licensed under Apache-2.0, whose §4(d) requires a redistributor to
carry forward the notices above; class-variance-authority ships under the same terms.

### ISC

Applies to `lucide-react`:

```text
ISC License

Copyright (c) for portions of Lucide are held by Cole Bemis 2013-2022 as part of Feather (MIT). All other copyright 2022-present Lucide Contributors.

Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted, provided that the above copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

