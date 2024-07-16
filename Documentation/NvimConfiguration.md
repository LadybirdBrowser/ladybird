# Neovim Project Configuration

There are two documented ways to support clangd in Neovim for the Ladybird
project:

1. With [nvim-lspconfig](https://github.com/neovim/nvim-lspconfig) (assumes existing setup)
2. With [coc.nvim](https://github.com/neoclide/coc.nvim) (from scratch, potentially out of date)

For both setups, make sure you ran `./Meta/ladybird.sh run ladybird` at least 
once.

## With nvim-lspconfig

> Note: This guide assumes Lua is being used, but can easily be adapted to 
> VimScript.

If you have an [nvim-lspconfig](https://github.com/neovim/nvim-lspconfig), setup
already, registering clangd is similar to other LSPs:
```lua
require('lspconfig').clangd.setup {
  -- If you have an on_attach function, this is where you'd put it. If not, 
  -- you can delete this line.
  -- on_attach = ...,

  -- This is where you'd put capabilities (i.e. if you're useing nvim-cmp)
  -- capabilities = ...,

  -- If you have another clangd installation, put it here. Note that we use 
  -- clangd version 18 for the Ladybird project.
  -- cmd = { '/path/to/clangd' },
}
```

A base `setup` call should be enough to register the LSP, but if false-negative
LSP errors occur upon opening Neovim, make sure your clangd installation is at
least version 18.

## With coc.nvim

> Note: This guide assumes VimScript is being used, but can easily be adapted to
> Lua. It also starts from scratch.

Neovim can be configured to use the [coc-clangd](https://github.com/clangd/coc-clangd)
plugin to provide code-completion as well as inline
[git blame](https://github.com/f-person/git-blame.nvim) using [vim-plug](https://github.com/junegunn/vim-plug).
Formatting can also be configured with [formatter.nvim](https://github.com/mhartington/formatter.nvim).

### Install vim-plug

See [https://github.com/junegunn/vim-plug](https://github.com/junegunn/vim-plug).

### Install coc.nvim

The config file for neovim is at `~/.config/nvim/init.vim` or if
set `$XDG_CONFIG_HOME/nvim/init.vim`.

Add the Plugin:

```vim
Plug 'neoclide/coc.nvim', { 'branch': 'release' }
```

Run `:PlugInstall` inside nvim.

### Install coc-clangd via CocInstall

```vim
:CocInstall coc-clangd
```

> **Note**: This guide is tested with clangd version 14.0.6 and 15.0.6.

In case you have not installed clangd already install it with
`:CocCommand clangd.install`.

This will install a separate version of clangd just for neovim.

### Configure coc-clangd in coc-settings.json

Use the following settings to ensure that coc-clangd works out of the box.

> **Note**: You might want to adjust the `clangd.fallbackFlags` depending on your build
system and customize the `inlayHints.sep` based on your preference.

```json
{
    "clangd.fallbackFlags": ["-std=c++23"],
    "semanticTokens.enable": true,
    "inlayHint.subSeparator": "︴",
    "inlayHints.enableParameter": true,
    "clangd.inlayHints.sep": "⇝"
}
```

To change the coc-settings.json go to the file `~/.config/nvim/coc-settings.json`
or type `:CocConfig` in the command line.

> **Note**: In case you already had another c++ language server configured in the
`coc-settings.json` you might want to nuke it first and
work towards your desired config by adding the other parts back in to avoid
conflicts.

> **Note**: If you have configured `clangd` as a languageServer in
`coc-settings.json`, you should remove it to avoid running clangd twice!

> **Note**: `clangd.inlayHints.sep` breaks on `clangd 15.0.6`.

### Formatting
For code formatting the formatter plugin can be used.
```vim
Plug 'mhartington/formatter.nvim'
```
#### Configuration
To use the formatter plugin one needs to opt-in to specific formatters. An example lua configuration which uses clang-format for cpp files:
```lua
require("formatter").setup{
    filetype = {
        cpp = {
            require("formatter.filetypes.cpp").clangformat
        }
    }
}
```

### Install git blame (Optional)

```vim
Plug 'f-person/git-blame.nvim'
```

Run `:PlugInstall` inside nvim.

### Configure your init.vim

The config file for neovim is at `~/.config/nvim/init.vim` or if
set `$XDG_CONFIG_HOME/nvim/init.vim`.

The `init.vim` excerpt:

```vim
"IMPORTANT: the leader key for <leader> keycombos
let mapleader = "\\"

"BEGIN: git blame (optional)
hi GitBlame guifg=#7b7b7b
let g:gitblame_date_format = '%d.%m.%y %H:%M'
let g:gitblame_highlight_group = 'GitBlame'
let g:gitblame_message_when_not_committed = 'You: Uncommitted changes'
let g:gitblame_message_template = '   <author> (<committer>), <date> <sha> • <summary>'
"END: git blame

"BEGIN: coc
"inline hints (depending on clangd version one or another gets used)
hi CocHintVirtualText guifg=#84afe0
hi CocInlayHint guifg=#84afe0 guibg=#393939
hi CocInlayHintParameter guifg=#84afe0 guibg=#393939
hi CocInlayHintType guifg=#89ddff guibg=#393939

"semantic highlighting
hi CocSemMethod guifg=#bfaa87 gui=bold
hi CocSemFunction guifg=#bfaaf7 gui=bold
hi CocSemParameter guifg=#a9bfd1 gui=underline
hi CocSemVariable guifg=#8edbdb
hi CocSemProperty guifg=#23ce6d
hi link CocSemEnumMember Constant
hi link CocSemEnum CocSemClass
hi Constant guifg=#f78c6c
hi CocSemClass guifg=#89ddff
hi Statement guifg=#c792ea
hi Type guifg=#db954a


"remap keys for applying refactor code actions (on warnings) (\re)
nmap <silent> <leader>re <Plug>(coc-codeaction-refactor)
xmap <silent> <leader>r  <Plug>(coc-codeaction-refactor-selected)
nmap <silent> <leader>r  <Plug>(coc-codeaction-refactor-selected)

"outline for file (\o)
nmap <silent><nowait> <leader>o :<C-u>CocList outline<cr>

"goto definition etc.
nmap <silent> gd <Plug>(coc-definition)
nmap <silent> gt <Plug>(coc-type-definition)
nmap <silent> gi <Plug>(coc-implementation)
nmap <silent> gr <Plug>(coc-references)

"coc rename (\rn)
nmap <leader>rn <Plug>(coc-rename)

"prev or next error
nmap <silent> [g <Plug>(coc-diagnostic-prev)
nmap <silent> ]g <Plug>(coc-diagnostic-next)

"confirm coc-suggestion with enter
imap <silent><expr> <CR> coc#pum#visible() ? coc#pum#confirm() : "\<CR>"

"ctrl+space for completion
imap <silent><expr> <c-space> coc#refresh()

"show documentation with ctrl+k
nmap <silent><c-k> :call ShowDocumentation()<CR>

"show documentation if it's available
function! ShowDocumentation()
  if CocAction('hasProvider', 'hover')
    call CocActionAsync('doHover')
  else
    call feedkeys('K', 'in')
  endif
endfunction

"coc-clangd switch between header and source
nmap <silent>gs :CocCommand clangd.switchSourceHeader vsplit<CR>
"END: coc
```
