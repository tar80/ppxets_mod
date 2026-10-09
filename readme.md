# ppxets_mod

PPx 用の Everything 検索モジュール [PPXETS](https://toroidj.github.io/slppx.html#ppxets) の改変・拡張版です。

PPc の一行編集と Whereis で、[Everything](https://www.voidtools.com/) の検索結果を利用できます。

## 特徴

- Everything(1.4, 1,5)に対応
- Migemo(1.8.0以降)によるローマ字検索(オプション)
- Everything が見つからない・応答しない場合の挙動を調整
- ETS64bit版のみ提供

## 導入

[Releases](../../releases) から zip をダウンロードできます。
設定や使い方などは同梱の `PPXETS_M.TXT` を参照してください。

## ビルド

`src` ディレクトリ内のファイルと、次のヘッダを使って nmake でビルドします。
詳細は `PPXETS_M.TXT` を参照してください。

- `PPCOMMON.H`, `TOROWIN.H` ... [ppxets05.zip](https://toroidj.github.io/slppx.html#ppxets) 内の `PPXETSRC.LZH`
- `everything_ipc.h` ... [Everything-SDK](https://www.voidtools.com/downloads/)

※これらのファイルは、配布元からダウンロードしてください。

## リンク

- [TORO's Library](https://toroidj.github.io/index.html)
- [voidtoods](https://www.voidtools.com/)
- [koron/cmigemo](https://github.com/koron/cmigemo)
