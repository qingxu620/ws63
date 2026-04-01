# ws63_test 软著申报材料包

## 1. 目录用途

这个目录用于给 `ws63_test` 激光打标工程准备中国软件著作权登记材料。这里的申报口径不是“整个 SDK 仓库”，而是聚焦 `ws63_test/` 下真正属于你们原创业务逻辑的软件部分。

当前建议的申报名称：

- 软件全称：`WS63双板激光打标控制软件`
- 软件简称：`WS63激光打标控制软件`
- 版本号：`V1.0`

如果你们公司内部已经有正式产品名，也可以在提交前统一替换。

## 2. 已包含的文件

- `application_info_draft.md`
  软著申请表信息草案，包含软件范围、功能简介、技术特点、运行环境和待人工补充项。
- `fill_required_info.md`
  最小回填信息模板，你把关键信息填给我后，我可以继续替你整理成最终版。
- `software_manual.md`
  面向软著提交的《软件说明书》草稿，可继续按代理机构或提交系统要求排版。
- `software_operation_manual.md`
  面向软著提交的长版《软件操作手册》，更偏安装、启动、使用、排障和日常维护。
- `submission_checklist.md`
  软著提交清单，把法规层面的材料要求和当前工程已有文件做了对应。
- `generate_code_material.py`
  源程序鉴别材料生成脚本，只提取 `ws63_test/common`、`ws63_test/transmitter`、`ws63_test/receiver` 中的原创业务代码。
- `out/source_code_material.txt`
  已生成的一版源程序鉴别材料草稿。
- `out/source_code_manifest.txt`
  生成源程序鉴别材料时所使用的文件清单和行数统计。

## 3. 建议申报范围

建议纳入软著的源代码范围：

- `ws63_test/common`
- `ws63_test/transmitter`
- `ws63_test/receiver`

建议不要纳入本次软著提交范围的内容：

- `ws63_test` 目录之外的芯片 SDK、OS 适配层、平台中间件
- 通用驱动、板级支撑、协议栈通用实现
- `open_source/` 下的第三方开源代码
- `ws63_test/tools/` 下的测试脚本

这样划分更稳妥，因为它和这个软件真正的原创点是一致的：G-Code 解析、星闪无线传输、命令队列管理、插补执行、激光功率控制、状态反馈和安全保护。

## 4. 推荐使用方式

1. 先审阅 `application_info_draft.md`，把权利人、完成日期、发表状态等真实信息补上。
2. 再审阅 `software_manual.md`，按你们最终申报名称和公司信息替换标题页内容。
3. 如果代码还有变动，重新生成源码材料：

```bash
python3 ws63_test/softcopyright/generate_code_material.py
```

4. 把 `out/source_code_material.txt` 导入 Word，使用等宽字体检查分页效果。
5. 按提交渠道要求导出 PDF 或打印盖章版本。

## 5. 当前结论

- 当前纳入申报范围的原创 `.c/.h` 文件共 `3420` 行。
- 按常见的“每页 50 行”口径，已超过 60 页，因此默认采用“前 30 页 + 后 30 页”的生成方式。
- 如果代理机构要求每页 45 行或别的模板，可以通过脚本参数调整，例如 `--lines-per-page 45`。
- 提交前请务必把软件完成日期、权利人名称、地址、联系人信息改成你们公司的正式资料。
