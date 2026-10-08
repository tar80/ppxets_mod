# ppxets_mod

PPx 用の Everything 検索モジュール [PPXETS](https://toroidj.github.io/ppx/) の更新・拡張版です。

PPx の一行編集の補完と PPc の Whereis で、[Everything](https://www.voidtools.com/) の検索結果を利用できます。

## 特徴

- Everything 1.4 以降に対応
- Migemo によるローマ字検索(optional)
- Everything が見つからない・応答しない場合の待機を調整
- ETS64bit版のみの提供

## 導入

[Releases](../../releases) から zip をダウンロードできます。
設定や使い方などは同梱の `PPXETS_M.TXT` を参照してください。

## ビルド

`src` ディレクトリ内のファイルと、次のヘッダを使って nmake でビルドします。
詳細は `PPXETS_M.TXT` を参照してください。

※これらのファイルは、このリポジトリには含まれません。配布元からダウンロードしてください。

- `PPCOMMON.H`, `TOROWIN.H` ... [ppxets05.zip](https://toroidj.github.io/ppx/ppxets05.zip) 内の `PPXETSRC.LZH`
- `everything_ipc.h` ... [Everything SDK](https://www.voidtools.com/Everything-SDK.zip)

## リンク

- [PPx](https://toroidj.github.io/ppx/)
- [Everything](https://www.voidtools.com/)
- [C/Migemo](https://github.com/koron/cmigemo)
