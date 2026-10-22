# Corte Rápido ✂

Cortador de vídeo minimalista para Windows 10+. Um único `CorteRapido.exe` (~220 KB), sem instalar nada.

## Uso

1. Abra um vídeo (botão **Abrir vídeo**, arrastar o arquivo para a janela, ou `CorteRapido.exe video.mp4`).
2. **Arraste** na linha do tempo para selecionar o trecho (as bordas laranja podem ser ajustadas depois).
3. **Botão direito** no trecho → **Exportar este trecho…** → escolha onde salvar (abre na pasta do vídeo original).

| Atalho | Ação |
|---|---|
| Espaço / clique no vídeo | Play / pausa |
| Enter | Tocar só o trecho selecionado |
| ← / → | Quadro anterior / próximo (Shift ou Ctrl: 1 s) |
| I / O | Marcar início / fim no cursor |
| Esc | Limpar seleção (ou cancelar exportação) |
| Ctrl+O / Ctrl+E | Abrir / exportar |

O corte é preciso no quadro: o trecho é recodificado em H.264 + AAC (MP4), usando o encoder por hardware da GPU quando disponível, com taxa de bits igual à do original.

## Compilar

Requer Visual Studio 2019/2022 com "Desenvolvimento para desktop com C++".

```bat
build.bat
```

Só usa APIs do Windows (Win32 + Media Foundation), CRT estático.
Edições "N" do Windows precisam do *Media Feature Pack*.

`tools/test.cpp` é um harness de teste (gera vídeo sintético, exporta sem GUI, captura a janela).
