# Contributing to OpenCsp / 参与贡献

Thank you for helping improve the CSP reference code. 感谢参与 CSP 参考实现的改进。

## Reporting issues / 提交问题

Please include / 请尽量提供：

- OS, CPU architecture and compiler version / 操作系统、CPU 架构、编译器版本
- The exact command you ran and its full output / 执行的命令和完整输出
- Which viewer or SDK you used, with version, if the problem is about opening a file / 如果是打开文件的问题，请注明使用的阅片软件或 SDK 及版本
- If possible, a small `.csp` file or the header bytes (`xxd -l 128 file.csp`) / 如可能，附一个小的 `.csp` 文件或文件头 128 字节

Do not upload slides that contain patient information. 请勿上传含有患者信息的切片。

## Pull requests / 提交 PR

1. Build and run the tests locally before opening a PR. 提交前请在本地编译并运行测试：

   ```bash
   cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
   ```

2. Keep changes focused; one topic per PR. 每个 PR 只做一件事。
3. Changes to the file layout must follow `CSP数字病理图像数据存储格式规范260422.docx`; cite the section you rely on. 涉及文件格式的改动须符合规范文档，并注明依据的章节。
4. Add or update a test in `tests/` when you fix a bug. 修复问题时请在 `tests/` 中补充测试。
5. CI (GitHub Actions) builds and tests on Ubuntu, macOS and Windows (MinGW-w64); it must pass. CI 会在三个平台编译并测试，需全部通过。

## License / 许可

By contributing you agree that your contributions are licensed under the Apache License 2.0 (see `LICENSE`). 提交贡献即表示同意以 Apache-2.0 许可发布。
