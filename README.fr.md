*Ce projet a été créé avec l'aide de Claude Opus 5.5 (Anthropic).*

# WarbandVR

*[English version](README.md)*

Mod de réalité virtuelle pour **Mount & Blade: Warband**, au niveau du moteur. Il fonctionne
avec WSE2 64 bits (`mb_warband_wse2_x64.exe`) et rend jouable en VR **n'importe quel module**
(Native, Viking Conquest, Napoleonic Wars, mods de la communauté et du Steam Workshop…). La VR
ne passe jamais par les fichiers d'un module : une DLL est injectée dans le jeu au lancement.
Aucun fichier du jeu ni de WSE2 n'est modifié.

- Rendu stéréo (rendu alterné ou **vraie stéréo**), suivi de la tête et des mains, son dans
  le casque.
- Combat physique aux manettes : coups au geste, lame et bouclier toujours actifs, prise à
  deux mains des armes à deux mains et d'hast, tir à l'arc à deux mains.
- Menus, carte du monde, inventaire et dialogues sur un écran virtuel, au pointeur.
- Ordres aux troupes par un menu radial, pour tous les modules.
- **Lanceur** : module, langue du jeu, préréglages et tous les réglages (VR et graphismes de
  Warband) avant chaque partie.
- OpenXR : tout casque dont le runtime OpenXR est actif (Meta, SteamVR…), sous Windows ou sous
  Linux / SteamOS avec Proton (y compris le **Steam Frame en autonome**).

> **Conçu pour le solo.** Son utilisation dans des parties multijoueur publiques est
> déconseillée : il n'a été testé qu'en solo.

## Sommaire
1. [Prérequis](#prérequis)
2. [Installation](#installation)
3. [Lancer le jeu en VR](#lancer-le-jeu-en-vr)
4. [Configuration](#configuration)
5. [Commandes](#commandes)
6. [Fonctionnalités](#fonctionnalités)
7. [Limites connues](#limites-connues)
8. [Dépannage](#dépannage)
9. [Compiler depuis les sources](#compiler-depuis-les-sources)
10. [Licence et crédits](#licence-et-crédits)

## Prérequis

- **Windows 10/11 64 bits**, ou **Linux / SteamOS** avec Proton (version Windows du jeu).
- Mount & Blade: Warband avec **WSE2** (`mb_warband_wse2_x64.exe` dans le dossier du jeu).
- Un casque VR et son runtime **OpenXR** :
  - **Meta Quest** (Quest Link ou Air Link) : application Meta Horizon / Oculus, runtime
    OpenXR Meta actif ;
  - **Steam Frame, Valve Index, HTC Vive…** : SteamVR.
- Dans les options du jeu, **direction d'attaque : « mouvement de la souris »**. Le mod
  choisit la direction des coups en simulant un mouvement de souris.

Casques testés :

| Casque | Machine | Fréquence |
|---|---|---|
| Meta Quest 2 | PC Windows (Quest Link) | 120 Hz |
| Steam Frame, en streaming | PC Windows (SteamVR) | 90 Hz |
| Steam Frame, **en autonome** | Le casque lui-même (SteamOS ARM64, Proton 11) | 72 Hz |

Les autres casques OpenXR devraient fonctionner, car tout ce qui dépend du casque (résolution,
champ de vue, écart des yeux, fréquence, son) est lu depuis le runtime. Ils ne sont pas testés.

## Installation

Installez d'abord **WSE2** (Warband Script Enhancer 2) dans le dossier du jeu, en suivant ses
propres instructions : `mb_warband_wse2_x64.exe` doit se trouver à côté de `mb_warband.exe`.
Vérifiez que le jeu démarre avec WSE2 avant d'ajouter WarbandVR.

Téléchargez ensuite `WarbandVR-vX.Y.Z.zip` sur la page [Releases](../../releases) et décompressez-le.
Il contient trois fichiers à garder **dans un même dossier** :

| Fichier | Rôle |
|---|---|
| `warband_vr_injector.exe` | Démarre le jeu (après le lanceur) et y injecte la DLL |
| `warband_vr.dll` | Le mod |
| `warband_vr_config.exe` | Lanceur et outil de configuration (fenêtre) |

Le fichier de réglages `warband_vr.ini` est créé au premier lancement dans ce dossier, avec
les valeurs par défaut. Le journal `warband_vr.log` y est écrit à chaque session.

### Windows

**Conseillé :** décompressez les trois fichiers **dans le dossier du jeu**, à côté de
`mb_warband_wse2_x64.exe` (par défaut
`C:\Program Files (x86)\Steam\steamapps\common\MountBlade Warband`). Le jeu est alors trouvé
automatiquement. Seuls de nouveaux fichiers sont ajoutés : aucun fichier du jeu ni de WSE2
n'est remplacé.

Ils peuvent aussi rester dans n'importe quel autre dossier : le dossier du jeu se règle alors
dans le lanceur, onglet **Lancement**.

Au premier lancement, choisissez dans le lanceur le préréglage **PCVR** et le **runtime VR** :
*Par défaut de Windows* pour un Meta Quest en Quest Link / Air Link (runtime Meta actif),
*SteamVR* pour un casque passant par SteamVR (Steam Frame en streaming, Index, Vive…).

### Linux / SteamOS (Proton), Steam Frame en autonome

1. Dans Steam, propriétés de Warband → **Compatibilité** : forcez Proton (Proton 11 ARM64 sur
   le Steam Frame). La version Linux native du jeu n'a pas WSE2 : il faut la version Windows.
2. Installez WSE2 dans le dossier du jeu, comme sous Windows.
3. Copiez les trois fichiers de WarbandVR **dans le dossier du jeu**
   (`~/.local/share/Steam/steamapps/common/MountBlade Warband`).
4. Copiez aussi dans le dossier du jeu `d3dx9_43.dll` et `D3DCompiler_43.dll` d'une
   installation Windows (DirectX). Sans eux, WSE2 plante en compilant ses post-effets sous
   Proton.
5. Options de lancement Steam du jeu :
   ```
   export WINEDLLOVERRIDES="d3dx9_43,d3dcompiler_43=n,b"; eval $(echo "%command%" | sed "s|mb_warband.exe|warband_vr_injector.exe|")
   ```
6. Au premier lancement, choisissez le préréglage **Steam Frame (autonome)** dans le lanceur.

Le runtime OpenXR est celui de Steam (SteamVR via `wineopenxr`) : laissez le réglage « Runtime
VR (OpenXR) » sur « Par défaut de Windows ».

## Lancer le jeu en VR

1. Branchez et allumez le casque, puis démarrez son logiciel (Quest Link ou SteamVR). Pour un
   casque sans fil (Steam Frame en streaming…), connectez-le au PC. En autonome, il n'y a rien
   à faire.
2. Lancez **`warband_vr_injector.exe`** (sous Proton : lancez le jeu depuis Steam). Le
   **lanceur** s'ouvre : choisissez le module, la langue du jeu, un préréglage, ajustez les
   réglages si besoin, puis cliquez sur **JOUER**. *Quitter* annule le lancement.
3. Mettez le casque : les menus apparaissent sur un écran virtuel devant vous.

Le mod attend le casque s'il n'est pas encore prêt : l'ordre n'a pas d'importance. Pour
démarrer sans lanceur, décochez **« Afficher le lanceur au démarrage »** (onglet Lancement).

### Options de l'injecteur

```
warband_vr_injector.exe [--game-dir <dossier>] [--steamvr | --runtime <json> | --default-runtime]
                        [--launcher | --no-launcher] [-- <arguments du jeu>]
```

| Option | Effet |
|---|---|
| `--game-dir <dossier>` | Dossier du jeu. Par défaut : le dossier de l'injecteur s'il contient le jeu, sinon le réglage « Dossier du jeu » |
| `--steamvr` | Utilise SteamVR pour ce lancement, sans changer le runtime OpenXR de Windows |
| `--runtime <json>` | Utilise le manifeste OpenXR d'un autre runtime |
| `--default-runtime` | Force le runtime OpenXR de Windows |
| `--launcher` / `--no-launcher` | Ouvre (ou non) le lanceur, quel que soit le réglage |
| `-- <arguments>` | Arguments passés au jeu (pas de lanceur) ; par défaut `--module <réglage Module> --no-intro` |

### Jouer un autre module

Choisissez-le dans le lanceur (**Lancement → Module**) : la liste contient les modules du
dossier `Modules` du jeu et ceux du **Steam Workshop** auxquels vous êtes abonné, sous la forme
`Titre (Steam Workshop)`. Le lanceur demande leurs titres à Steam (connexion internet
nécessaire), et Steam doit être lancé pour y jouer. En ligne de commande :

```
warband_vr_injector.exe -- --module "Viking Conquest" --no-intro
```

## Configuration

Le lanceur (ouvert au démarrage) et **`warband_vr_config.exe`** (lancé seul, bouton
*Enregistrer* au lieu de *JOUER*) sont la même fenêtre. Elle est dessinée par l'outil
lui-même, en grand et en thème sombre, pour rester lisible dans un casque : **− / +**
changent la taille de l'interface, le bouton de langue passe de l'anglais au français. Les
listes s'ouvrent dans la fenêtre ; pointez un réglage pour voir son explication en bas.

Les réglages VR sont relus **en direct** par le jeu (environ 1 s) : pas besoin de relancer,
sauf pour le runtime VR. Les graphismes de Warband et la langue du jeu s'appliquent au
prochain lancement.

### Préréglages

| Préréglage | Contenu |
|---|---|
| **PCVR (défaut)** | Réglages VR validés au Quest 2 ; graphismes de Warband en **Ultra**, sans le flou de profondeur de champ |
| **Steam Frame (autonome)** | Vraie stéréo, échelle de rendu 0,85 ; graphismes de Warband en **Bas**, avec le sang au maximum (72 paires d'images par seconde validées au casque) |
| Les vôtres | *Enregistrer…* les range dans `warband_vr_presets\` (à côté du `.ini`) ; *Exporter…* écrit un fichier où vous voulez ; *Importer…* en ajoute un et l'applique ; *Supprimer* |

Choisir un préréglage l'applique aussitôt dans la fenêtre ; *JOUER* ou *Enregistrer*
l'écrit. Un préréglage ne touche ni au dossier du jeu, ni au module, ni à la langue du jeu, ni
au runtime VR. Les préréglages intégrés ne sont jamais appliqués d'office : c'est vous qui
choisissez.

### Onglet Graphismes (Warband)

Les réglages graphiques du jeu, écrits dans son fichier `rgl_config.ini`
(`Documents\Mount&Blade Warband WSE2`) ; seules les valeurs modifiées sont écrites. La
**qualité globale** Bas / Moyen / Haut / Ultra reprend exactement les valeurs des préréglages
des options vidéo du jeu. Suivent les réglages un par un : shaders, textures, filtrage, ombres
(scène, personnages, végétation), herbe, végétation, détail des personnages, HDR, exposition,
profondeur de champ, anticrénelage, reflets de l'eau, particules, éclairage dynamique, halo du
soleil, cadavres, ragdolls, sang, traînées d'armes, taille des batailles (en troupes).

En VR, chaque image est dessinée deux fois : des graphismes plus bas donnent une image plus
fluide. Fermez le jeu avant de changer ces réglages : il réécrit son fichier en quittant.

### Langue du jeu

Onglet Lancement, sous le module : les langues installées dans le dossier `languages` du jeu.
Elle est écrite dans `%APPDATA%\Mount&Blade Warband WSE2\language.txt`, et s'applique au
prochain lancement, pour les modules qui proposent cette langue.

### Réglages VR

| Onglet | Réglage | Défaut |
|---|---|---|
| Lancement | Dossier du jeu | `C:\Program Files (x86)\Steam\steamapps\common\MountBlade Warband` (sous Proton : le dossier de l'outil) |
| | **Module** (modules installés et du Steam Workshop) | Native |
| | Afficher le lanceur au démarrage | Oui |
| | **Runtime VR (OpenXR)** : par défaut de Windows / SteamVR | Par défaut de Windows |
| Affichage | Mode de roulis (tête inclinée) | Compositeur inversé |
| | Écran virtuel : distance / largeur | 2 m / 2,4 m |
| | HUD : distance / largeur | 4 m / 46,9° |
| | Fenêtre du jeu à la résolution du casque | Oui |
| | **Échelle de rendu** (résolution par œil / résolution recommandée) | 1 |
| | Limite d'images du jeu (0 = automatique, calée sur le casque) | 0 |
| Contrôles | Rotation au stick (degrés/s) | 143 |
| | **Mode de rotation** : continue / par crans | Continue |
| | Rotation par crans : cran | 30° |
| | Zone morte du stick de rotation | 0,2 |
| | Seuil du saut (stick droit vers le haut) | 0,7 |
| | Source du réticule au démarrage : regard / main gauche / main droite | Regard |
| Combat | Attaques au geste | Oui |
| | Geste : vitesse de déclenchement / durée de mesure / déplacement minimal | 1 m/s / 50 ms / 5 cm |
| | Estoc : dominance du mouvement avant | 1,5 |
| | Arme de mêlée toujours active | Oui |
| | Arme active : vitesse de début / de fin | 1,5 / 0,8 m/s |
| | Puissance par m/s de main / puissance maximale | 1 / 4 |
| | Bouclier toujours actif / rayon de protection | Oui / 35 cm |
| | Torse de face (armes de mêlée) | Oui |
| | Arc : allonge complète (écart des mains) | 0,5 m |
| | Arc : précision maximale | 110 % |
| Avancé | Visée : gain de l'asservissement | 0,5 |
| | **Vraie stéréo** (expérimental) | Non |
| | Diagnostics : œil droit en premier, image gauche dans les deux yeux, temps par image dans le log | Non |

## Commandes

Les commandes sont données pour les manettes **Meta Touch**. Les Valve Index, HTC Vive et
Windows Mixed Reality ont des associations équivalentes. SteamVR présente les manettes du
Steam Frame comme des Touch, et permet aussi de réassigner les boutons.

### Menus, carte du monde, inventaire (écran virtuel)

| Bouton | Action |
|---|---|
| Gâchette (une des deux mains) | Clic gauche ; la main qui clique devient celle qui pointe |
| Y (gauche) / B (droite) | Clic droit |
| Stick haut / bas | Molette (défilement, zoom de la carte) |
| Stick gauche / droite | Tourner la caméra de la carte du monde |
| Poignée (une des deux mains) | Ctrl maintenu (Ctrl+clic dans l'inventaire) |
| X ou Menu (gauche), appui court | Échap |
| X ou Menu (gauche), appui long (~1 s) | **Recentrer la vue** |
| Clic du stick gauche | Referme la carte des ordres |

Quand le jeu demande du texte (nom du personnage, par exemple), un **clavier virtuel**
apparaît sous l'écran, dans la disposition de votre clavier Windows.

### Combat, villes, déplacements

| Main gauche | Action |
|---|---|
| Stick | Se déplacer |
| Clic du stick | **Menu radial des ordres** |
| Gâchette | Bloquer |
| Poignée | Utiliser (parler, ouvrir, ramasser…) |
| X | Appui court : Échap ; tenu 1 s : source du réticule (regard / main gauche / main droite) ; dans le menu radial : retour |
| Y | Menu Tab (quitter la bataille…) |
| Menu | Appui court : Échap ; appui long : recentrer (sous SteamVR, ce bouton ouvre le tableau de bord de SteamVR : utilisez X) |

| Main droite | Action |
|---|---|
| Stick gauche / droite | Tourner le corps (continu ou par crans) |
| Stick vers le haut | Sauter |
| Clic du stick | Coup de pied (appui court) ; vue 1re / 3e personne (tenu 1 s) |
| A | Arme suivante (appui court) ; mode de l'arme (appui long : baïonnette, une / deux mains, lance couchée…, touche X du jeu) |
| B | Mettre ou retirer le bouclier |
| Gâchette | Bander l'arc / tirer (armes à distance) ; armes à deux mains et d'hast : tenue = **prise à deux mains** |
| Poignée | Arc et armes à distance : comme la gâchette ; autres armes : attaque de secours |
| **Geste** | Coups d'armes de mêlée : taille gauche, taille droite, coup par le haut, estoc |

### Attaquer selon l'arme

| Arme | Comment |
|---|---|
| Mêlée (épée, masse, lance…) | Un geste vif de la main droite donne un coup dans sa direction. La lame est aussi **toujours active** : elle touche dès qu'elle traverse un ennemi assez vite (une fois par mouvement), avec une puissance selon la vitesse de la lame. Le type de coup (estoc, par le haut, taille gauche ou droite) suit le vrai mouvement de la lame |
| Épée à deux mains, arme d'hast | Pareil, et en plus : **gâchette droite tenue** pour tenir l'arme à deux mains. Elle suit alors l'axe de vos deux manettes (du côté où pointe la manette droite), la main gauche se pose sur la poignée |
| Bouclier | Suit la main gauche ; il pare tout ce qui le touche, sans tenir la défense |
| Arc | Gâchette ou poignée droite tenue = bander, relâchée = tirer. La flèche suit l'axe de vos **deux mains** ; plus elles sont écartées, plus le tir est précis |
| Arbalète, pistolet, mousquet, armes de jet | Gâchette ou poignée droite tenue = viser, relâchée = tirer ou lancer. Le réticule passe automatiquement sur la main droite. Pas de tir au geste |

### Menu radial des ordres (clic du stick gauche)

1. Le **clic du stick gauche** ouvre un disque devant vos yeux, et un nouveau clic le referme.
2. **Poussez le stick gauche** vers une entrée, puis **relâchez-le** pour la valider. **X** :
   niveau précédent.
3. Premier niveau : *Toutes les troupes*, *Infanterie*, *Archers*, *Cavalerie*, *Carte des
   ordres*.
4. Niveaux suivants : les ordres **du jeu lui-même** (mouvement, formation, tir…), avec ses
   libellés, dans sa langue et pour n'importe quel module.
5. **Tenez cette position** : le disque se ferme, le jeu affiche son drapeau là où pointe
   votre main gauche, et le **clic du stick gauche** valide la position.

La *carte des ordres* (vue stratégique) n'existe que si le module la propose : dans Native,
en campagne, pas en bataille personnalisée.

### Touches de débogage (clavier)

| Touche | Effet |
|---|---|
| F9 | Change le mode de roulis (pour la session) |
| F10 | Active ou désactive l'arme de mêlée toujours active |
| F11 | Capture d'image (scène et interface) en PNG à côté de la DLL |

## Fonctionnalités

- **Rendu stéréo**, chaque œil avec sa caméra (position et champ de vue du casque) :
  - **rendu alterné** (par défaut) : un œil par image du jeu, deux images par rafraîchissement
    du casque, avec la même pose de tête pour les deux yeux d'une paire ;
  - **vraie stéréo** (réglage *Vraie stéréo*) : les deux yeux à chaque image du jeu. La
    simulation, les ombres et l'interface ne sont calculées qu'une fois par paire : relief
    juste en mouvement, et jusqu'à deux fois plus d'images quand le jeu est limité par le
    processeur.
- **Cadence** calée sur le casque : une paire d'images par rafraîchissement ; si la machine ne
  suit pas, une paire toutes les 2 ou 3 rafraîchissements, avec des essais réguliers pour
  remonter dès que possible. La limite d'images des options du jeu est ignorée en VR.
- **Résolution** : la fenêtre du jeu prend la résolution recommandée par le casque,
  multipliée par l'*échelle de rendu*.
- **HUD** (santé, munitions, noms) sur un plan lié à la tête, lisible et sans dédoublement.
- **Menus** : écran virtuel fixe, pointeur laser, manettes affichées en 3D, clavier virtuel.
- **Corps VR** : en 1re personne (vue par défaut au début de chaque mission), les bras du
  personnage suivent vos mains. L'arme tenue suit la poignée de la manette ; les armes à
  distance prennent la pose du jeu pendant la visée, le tir et le rechargement. Avec une arme
  de mêlée, le haut du corps est de face au lieu des postures de profil du jeu.
- **Combat physique** : arme et bouclier toujours actifs (type de coup selon le mouvement de
  la lame, un coup par mouvement), prise à deux mains, coups au geste, puissance selon la
  vitesse de la lame, visée asservie (regard, main gauche ou main droite), coup de pied, mode de l'arme
  (baïonnette…).
- **Tir à l'arc VR** : visée par l'axe des deux mains, précision selon l'allonge.
- **Ordres** : menu radial universel, « tenez cette position » au drapeau.
- **Son** : envoyé sur la sortie audio du casque, donnée par le runtime (Meta) ou sortie par
  défaut (SteamVR).
- **Confort** : recentrage, rotation continue ou par crans.
- **Lanceur** : module (dont Steam Workshop), langue du jeu, préréglages, réglages VR et
  graphismes de Warband.
- **Robustesse** : aucune adresse mémoire codée en dur, tout est retrouvé par signature. Si
  une mise à jour de WSE2 casse une signature, la fonction concernée est désactivée et le log
  le dit, sans planter le jeu.

## Limites connues

- **Prise à deux mains** : la main gauche ne se pose sur l'arme que tant que la gâchette droite
  est tenue ; pour les coups au geste, le jeu joue toujours ses propres animations.
- **Vraie stéréo** : expérimentale, validée sur le Steam Frame en autonome et sur PC avec le
  Steam Frame en streaming (désactivée dans le préréglage PCVR).
- **Cadence par paires et vraie stéréo** : validées sur le Steam Frame en autonome et en
  streaming ; pas encore revalidées au Quest 2.
- **S'accroupir** : non associé (désactivé dans Native).
- **Carte des ordres** : n'existe que si le module la propose (dans Native : en campagne, pas
  en bataille personnalisée). Le menu radial, lui, fonctionne partout.
- **Linux** : testé sur le Steam Frame en autonome (SteamOS ARM64, Proton 11). Les autres PC
  Linux et le Steam Deck ne sont pas testés.
- **Multijoueur** : non pris en charge ; déconseillé dans les parties multijoueur publiques.

## Dépannage

Tout est consigné dans **`warband_vr.log`**, à côté de la DLL (en français). Les lignes sont
préfixées par module (`[xr]`, `[render]`, `[engine]`, `[input]`). Le lanceur écrit
`warband_vr_injector.log` au même endroit.

| Problème | Piste |
|---|---|
| Pas d'image dans le casque | Le logiciel du casque est-il lancé ? Le log dit « runtime OpenXR pas prêt » tant que le casque n'est pas connecté |
| Le jeu démarre sur le mauvais casque / runtime | Réglage « Runtime VR (OpenXR) », ou options `--steamvr` / `--default-runtime` |
| Pas de son dans le casque | Lignes `son :` du log : liste des sorties FMOD et sortie choisie |
| Image saccadée | Le log indique la cadence (« une paire d'images par frame du casque », « le jeu ne tient pas… », « cadence remontée »). Baissez les graphismes de Warband ou l'*échelle de rendu* ; activez *Diagnostic : temps par image* pour voir qui limite (jeu, rendu, carte graphique) |
| Image floue ou pixelisée | Ligne « résolution de rendu par œil » du log ; *échelle de rendu* trop basse |
| Le lanceur ne s'ouvre pas | Réglage « Afficher le lanceur au démarrage », ou `warband_vr_injector.exe --launcher` |
| Liste des modules vide | Onglet Lancement : « Dossier du jeu » ; les titres du Workshop demandent internet |
| La langue du jeu ne change pas | Le module doit proposer cette langue (dossier `languages` du module) |
| Les coups partent dans la mauvaise direction | Option du jeu « direction d'attaque : mouvement de la souris » ; réglages *Geste* |
| Une fonction ne marche plus après une mise à jour de WSE2 | Cherchez « signature » dans le log |

## Compiler depuis les sources

Le code est en C++20, compilé avec CMake. Les dépendances (MinHook, chargeur OpenXR) sont
liées statiquement, avec le CRT : la DLL ne dépend d'aucune DLL tierce. Les trois fichiers
sont les mêmes quelle que soit la machine de compilation.

### Sous Windows (MSVC + vcpkg)

Il faut Visual Studio 2022, CMake et vcpkg (mode manifeste, triplet `x64-windows-static`).

```powershell
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

Les fichiers sont produits dans `build/Release/`.

### Sous Linux (compilation croisée clang-cl)

Fonctionne sur PC x86_64 et ARM64, y compris directement sur le Steam Frame (SteamOS). Il faut
`clang` (clang-cl), `lld` (lld-link), `cmake`, `ninja`, `git` et `curl`.

1. Une seule fois, le SDK Windows et le CRT MSVC (environ 630 Mo, dans `~/.local/opt/winsdk`),
   téléchargés par [xwin](https://github.com/Jake-Shadle/xwin). Le script accepte la licence de
   Microsoft (celle de Visual Studio Build Tools) :
   ```sh
   tools/setup_linux_toolchain.sh
   ```
2. Compilation (MinHook et le chargeur OpenXR sont téléchargés et compilés depuis leurs
   sources, dans les mêmes versions que vcpkg) :
   ```sh
   cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-xwin.cmake
   cmake --build build-linux
   ```

Les fichiers sont produits dans `build-linux/`. Seule la configuration Release est possible
(le CRT de débogage n'est pas extrait). Le dossier du SDK se change avec la variable
`XWIN_DIR`.

### Organisation du code

| Dossier | Contenu |
|---|---|
| `injector/` | Injecteur 64 bits (lanceur, CreateProcess + injection LoadLibrary) |
| `src/render/` | Hooks Direct3D 9 (Present, cibles de rendu), partage des images vers D3D11, fenêtre du jeu, mesure des temps |
| `src/xr/` | Session OpenXR, cadence, manettes, rendu des manettes, clavier virtuel, menu radial |
| `src/engine/` | Moteur du jeu : caméra stéréo, vraie stéréo, squelette (bras VR), combat, ordres, saisie de texte, son, cadence |
| `src/input/` | Injection DirectInput (clavier, souris), pointeur des menus |
| `src/util/` | Journal, recherche de signatures, réglages et préréglages, détours (MinHook, tables virtuelles) |
| `config_tool/` | Lanceur et outil de configuration (interface GDI), graphismes et langue de Warband |
| `cmake/` | Compilation croisée depuis Linux (clang-cl, lld-link, xwin) |
| `tools/` | Installation de la compilation Linux, exploration du PDB du jeu sans cdb (`pdbtool.py`), scripts d'analyse |

## Licence et crédits

WarbandVR est distribué sous [licence MIT](LICENSE).

- Bibliothèques tierces liées dans les binaires : **MinHook** (BSD 2-Clause), **chargeur du
  SDK OpenXR** (Apache 2.0), **JsonCpp** (MIT) : voir `THIRD_PARTY_NOTICES.txt` dans la
  release.
- Architecture inspirée de **WSE** (Warband Script Enhancer, WTFPL) ; WarbandVR fonctionne
  par-dessus **WSE2**, qu'il ne modifie pas.
- Mount & Blade: Warband est un jeu de TaleWorlds Entertainment. Ce projet n'est ni affilié
  à TaleWorlds ou aux auteurs de WSE2, ni approuvé par eux.
