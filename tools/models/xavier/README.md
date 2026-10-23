# Xavier for God Hand

`assets/characters/xavier/` (the model nodes and textures) was made from `Xavier_v1_1_1-README.blend` (Xavier:
Renegade Angel, used with permission) with these scripts, Blender 5.2 as a Python module (`pip install bpy==5.2.3
pillow`), numpy and Pillow:

1. `prep1.py`: Xavier's mesh (shape keys applied, rest pose) moved onto Gene's skeleton: uniform scale to Gene's height
   (top of the head 1.837), legs and the snake arm aimed along Gene's bones, the right arm removed (the game's God Hand
   arm takes its place; the shoulder pad covers the seam), the human mouth interior dropped, Xavier's deform bones
   mapped to Gene's 32 bones.
2. `prep2.py`: decimated to Gene's budget, the body atlas split into UV-island groups (fur by colour, the rest by place);
   the two loincloth panels are their own group.
3. `encode_xavier.py`: 4-bit, 16-colour TIM3 textures and triangle-strip vertex buffers, written as
   `xavier_nodes.scr` (an MD with an empty bone table), `xavier_coat.scr` (the loincloth panels, weighted to the front
   and back bone chains of Gene's coat tails in pl/pl01.dat, so the game's cloth motion swings them) and
   `xavier_textures.tm3`. The shoulder pad is enlarged (x1.3 around the upper arm) to cover the God Hand arm's shoulder.

Environment: `XAVIER_BLEND`, `GENE_PL00` (the game's pl/pl00.dat), `XAVIER_WORK` (work folder; Xavier's packed
textures saved as PNG in `<work>/textures`). The game's skeleton, motions, God Hand arm and the merge into pl00.dat
happen at build time from your disc (`tools/models/build_characters.py`), so nothing of the game is in the repository.
