# Third-party notices

Hark's original source and tests are licensed under **GPL-3.0-or-later**.
The full GPL version 3 is in [LICENSE](LICENSE).

* **yyjson 0.12.0**, Copyright (c) 2020 YaoYuan <ibireme@gmail.com>, is
  vendored unmodified under the MIT license. See
  [vendor/yyjson/LICENSE](vendor/yyjson/LICENSE) and
  [upstream](https://github.com/ibireme/yyjson/tree/0.12.0).
* **Isocline 1.1.0**, Copyright (c) 2021 Daan Leijen and the notices preserved in
  individual source files, is vendored under the MIT license. See
  [vendor/isocline/LICENSE](vendor/isocline/LICENSE) and
  [upstream](https://github.com/daanx/isocline/tree/v1.1.0).
* **libcurl** is linked from the host or cross toolchain, and is not vendored.
  It has a permissive [curl license](https://curl.se/docs/copyright.html).
  Its TLS backend and other enabled features have their own licenses. When
  distributing binaries, include the applicable notices and satisfy the licenses
  of the particular libcurl build and its dependencies.

The device-code flow interoperates with the public Codex OAuth client. Its
protocol was checked against OpenAI's official documentation and public Codex
source; no Codex source code is incorporated in this project.
