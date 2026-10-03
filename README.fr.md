# WarbandVR

*[English version](README.md)*

Mod de réalité virtuelle pour **Mount & Blade: Warband**, au niveau du moteur. Il fonctionne
avec WSE2 64 bits (`mb_warband_wse2_x64.exe`) et rend jouable en VR **n'importe quel module**
(Native, Viking Conquest, Napoleonic Wars, mods de la communauté…). La VR ne passe jamais par
les fichiers d'un module : une DLL est injectée dans le jeu au lancement. Aucun fichier du jeu
ni de WSE2 n'est modifié.

- Rendu stéréo, suivi de la tête et des mains, son dans le casque.
- Combat physique aux manettes : coups au geste, lame et bouclier toujours actifs, tir à l'arc
  à deux mains.
- Menus, carte du monde, inventaire et dialogues sur un écran virtuel, au pointeur.
- Ordres aux troupes par un menu radial, pour tous les modules.
- OpenXR : tout casque dont le runtime OpenXR est actif (Meta, SteamVR…).

> **Solo uniquement.** N'utilisez jamais le mod sur un serveur multijoueur.

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

- Windows 10/11 64 bits.
- Mount & Blade: Warband avec **WSE2** (`mb_warband_wse2_x64.exe` dans le dossier du jeu).
- Un casque VR et son runtime **OpenXR** :
  - **Meta Quest** (Quest Link ou Air Link) : application Meta Horizon / Oculus, runtime
    OpenXR Meta actif ;
  - **Steam Frame, Valve Index, HTC Vive…** : SteamVR.
- Dans les options du jeu, **direction d'attaque : « mouvement de la souris »**. Le mod
  choisit la direction des coups en simulant un mouvement de souris.

Casques testés : **Meta Quest 2** (Quest Link, 120 Hz) et **Steam Frame** (SteamVR, 90 Hz).
Les autres casques OpenXR devraient fonctionner, car tout ce qui dépend du casque (résolution,
champ de vue, écart des yeux, fréquence, son) est lu depuis le runtime. Ils ne sont pas testés.

## Installation

Téléchargez `WarbandVR-vX.Y.Z.zip` sur la page [Releases](../../releases) et décompressez-le.
Gardez ces trois fichiers **dans un même dossier**, n'importe où (pas besoin du dossier du jeu) :

| Fichier | Rôle |
|---|---|
| `warband_vr_injector.exe` | Lanceur : démarre le jeu et y injecte la DLL |
| `warband_vr.dll` | Le mod |
| `warband_vr_config.exe` | Outil de configuration (fenêtre) |

Le fichier de réglages `warband_vr.ini` est créé au premier lancement dans ce dossier, avec
les valeurs par défaut. Le journal `warband_vr.log` y est écrit à chaque session.

## Lancer le jeu en VR

1. Branchez et allumez le casque, puis démarrez son logiciel (Quest Link ou SteamVR).
   Pour un casque sans fil (Steam Frame…), connectez-le au PC.
2. Lancez **`warband_vr_injector.exe`**. Il démarre le jeu (module Native par défaut) et
   injecte le mod.
3. Mettez le casque : les menus apparaissent sur un écran virtuel devant vous.

Le mod attend le casque s'il n'est pas encore prêt : l'ordre n'a pas d'importance.

### Options du lanceur

```
warband_vr_injector.exe [--game-dir <dossier>] [--steamvr | --runtime <json> | --default-runtime]
                        [-- <arguments du jeu>]
```

| Option | Effet |
|---|---|
| `--game-dir <dossier>` | Dossier du jeu, si ce n'est pas celui de Steam par défaut (`C:\Program Files (x86)\Steam\steamapps\common\MountBlade Warband`) |
| `--steamvr` | Utilise SteamVR pour ce lancement, sans changer le runtime OpenXR de Windows |
| `--runtime <json>` | Utilise le manifeste OpenXR d'un autre runtime |
| `--default-runtime` | Force le runtime OpenXR de Windows |
| `-- <arguments>` | Arguments passés au jeu ; par défaut `--module Native --no-intro` |

Sans option de runtime, le lanceur suit le réglage **« Runtime VR (OpenXR) »** de l'outil de
configuration.

Exemple pour un autre module :

```
warband_vr_injector.exe -- --module "Viking Conquest" --no-intro
```

## Configuration

Lancez **`warband_vr_config.exe`**. Il est en anglais par défaut ; le français se choisit en
haut de la fenêtre. Chaque réglage est expliqué dans la fenêtre. Les valeurs par défaut sont
celles validées au casque, et le bouton *Valeurs par défaut* y revient. Les réglages sont
relus **en direct** par le jeu (environ 1 s) : pas besoin de relancer, sauf pour le runtime VR.

| Groupe | Réglage | Défaut |
|---|---|---|
| Affichage | Mode de roulis (tête inclinée) | Compositeur inversé |
| | Écran virtuel : distance / largeur | 2 m / 2,4 m |
| | HUD : distance / largeur | 4 m / 46,9° |
| | Fenêtre du jeu à la résolution du casque | Oui |
| | **Runtime VR (OpenXR)** : par défaut de Windows / SteamVR | Par défaut de Windows |
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
| | Arc : allonge complète (écart des mains) | 0,5 m |
| | Arc : précision maximale | 110 % |
| Avancé | Visée : gain de l'asservissement | 0,5 |

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
| Menu (gauche), appui court | Échap |
| Menu (gauche), appui long (~1 s) | **Recentrer la vue** |
| X | Referme la carte des ordres |

Quand le jeu demande du texte (nom du personnage, par exemple), un **clavier virtuel**
apparaît sous l'écran, dans la disposition de votre clavier Windows.

### Combat, villes, déplacements

| Main gauche | Action |
|---|---|
| Stick | Se déplacer |
| Clic du stick | Source du réticule (regard / main gauche / main droite) ; dans le menu radial : retour |
| Gâchette | Bloquer |
| Poignée | Utiliser (parler, ouvrir, ramasser…) |
| X | **Menu radial des ordres** |
| Y | Menu Tab (quitter la bataille…) |
| Menu | Appui court : Échap ; appui long : recentrer |

| Main droite | Action |
|---|---|
| Stick gauche / droite | Tourner le corps (continu ou par crans) |
| Stick vers le haut | Sauter |
| Clic du stick | Vue 1re / 3e personne |
| A | Arme suivante |
| B | Mettre ou retirer le bouclier |
| Gâchette | Bander l'arc / tirer (armes à distance) |
| Poignée | Arc et armes à distance : comme la gâchette ; autres armes : attaque de secours |
| **Geste** | Coups d'armes de mêlée : taille gauche, taille droite, coup par le haut, estoc |

### Attaquer selon l'arme

| Arme | Comment |
|---|---|
| Mêlée (épée, masse, lance…) | Un geste vif de la main droite donne un coup dans sa direction. La lame est aussi **toujours active** : elle touche dès qu'elle traverse un ennemi assez vite, avec une puissance selon la vitesse |
| Bouclier | Suit la main gauche ; il pare tout ce qui le touche, sans tenir la défense |
| Arc | Gâchette ou poignée droite tenue = bander, relâchée = tirer. La flèche suit l'axe de vos **deux mains** ; plus elles sont écartées, plus le tir est précis |
| Arbalète, pistolet, mousquet, armes de jet | Gâchette ou poignée droite tenue = viser, relâchée = tirer ou lancer. Le réticule passe automatiquement sur la main droite. Pas de tir au geste |

### Menu radial des ordres (X)

1. **X** ouvre un disque devant vos yeux, et un nouvel appui le referme.
2. **Poussez le stick gauche** vers une entrée, puis **relâchez-le** pour la valider. **Clic
   du stick** : niveau précédent.
3. Premier niveau : *Toutes les troupes*, *Infanterie*, *Archers*, *Cavalerie*, *Carte des
   ordres*.
4. Niveaux suivants : les ordres **du jeu lui-même** (mouvement, formation, tir…), avec ses
   libellés, dans sa langue et pour n'importe quel module.
5. **Tenez cette position** : le disque se ferme, le jeu affiche son drapeau là où vous
   regardez, et **X** valide la position.

La *carte des ordres* (vue stratégique) n'existe que si le module la propose : dans Native,
en campagne, pas en bataille personnalisée.

### Touches de débogage (clavier)

| Touche | Effet |
|---|---|
| F9 | Change le mode de roulis (pour la session) |
| F10 | Active ou désactive l'arme de mêlée toujours active |
| F11 | Capture d'image (scène et interface) en PNG à côté de la DLL |

## Fonctionnalités

- **Rendu stéréo** : chaque œil a sa caméra (position et champ de vue du casque), en rendu
  alterné. La cadence du jeu se cale sur le casque : deux images par rafraîchissement, une
  par œil (180 images/s à 90 Hz). Si la machine ne suit pas, elle passe à une image par
  rafraîchissement.
- **Résolution** : la fenêtre du jeu prend la résolution recommandée par le casque.
- **HUD** (santé, munitions, noms) sur un plan lié à la tête, lisible et sans dédoublement.
- **Menus** : écran virtuel fixe, pointeur laser, manettes affichées en 3D, clavier virtuel.
- **Corps VR** : en 1re personne (vue par défaut au début de chaque mission), les bras du
  personnage suivent vos mains. L'arme tenue suit la poignée de la manette ; les armes à
  distance prennent la pose du jeu pendant la visée, le tir et le rechargement.
- **Combat physique** : arme et bouclier toujours actifs, coups au geste, puissance selon la
  vitesse, visée asservie (regard, main gauche ou main droite).
- **Tir à l'arc VR** : visée par l'axe des deux mains, précision selon l'allonge.
- **Ordres** : menu radial universel, « tenez cette position » au drapeau.
- **Son** : envoyé sur la sortie audio du casque, donnée par le runtime (Meta) ou sortie
  Windows par défaut (SteamVR).
- **Confort** : recentrage, rotation continue ou par crans.
- **Robustesse** : aucune adresse mémoire codée en dur, tout est retrouvé par signature. Si
  une mise à jour de WSE2 casse une signature, la fonction concernée est désactivée et le log
  le dit, sans planter le jeu.

## Limites connues

- **Armes à deux mains, armes d'hast** : elles se manient au geste, sans prise réaliste à
  deux mains.
- **Rendu alterné** : chaque œil est rafraîchi une image sur deux. C'est fluide à cadence
  régulière, mais moins qu'un vrai rendu simultané des deux yeux.
- **S'accroupir** : non associé (désactivé dans Native).
- **Carte des ordres** : dépend du module (voir plus haut). Le menu radial, lui, fonctionne
  partout.
- **Multijoueur** : non pris en charge, et à ne pas utiliser.
- **Linux (Proton / Wine)** : **non testé**. Le mod est conçu pour Windows. Sous Proton, les
  points incertains sont le partage d'images Direct3D 9 → Direct3D 11 sous DXVK et OpenXR
  via `wineopenxr`. Retours bienvenus, avec le fichier `warband_vr.log`.

## Dépannage

Tout est consigné dans **`warband_vr.log`**, à côté de la DLL. Les lignes sont préfixées par
module (`[xr]`, `[render]`, `[engine]`, `[input]`).

| Problème | Piste |
|---|---|
| Pas d'image dans le casque | Le logiciel du casque est-il lancé ? Le log dit « runtime OpenXR pas prêt » tant que le casque n'est pas connecté |
| Le jeu démarre sur le mauvais casque / runtime | Réglage « Runtime VR (OpenXR) », ou options `--steamvr` / `--default-runtime` |
| Pas de son dans le casque | Lignes `son :` du log : liste des sorties FMOD et sortie choisie |
| Image floue en tournant la tête | Le log doit dire « casque à N Hz : le jeu vise 2N images/s ». Si le jeu ne suit pas, baissez les options graphiques |
| Les coups partent dans la mauvaise direction | Option du jeu « direction d'attaque : mouvement de la souris » ; réglages *Geste* |
| Une fonction ne marche plus après une mise à jour de WSE2 | Cherchez « signature » dans le log |

## Compiler depuis les sources

Il faut Visual Studio 2022 (MSVC, C++20), CMake et vcpkg. Les dépendances (MinHook,
chargeur OpenXR) sont en mode manifeste, avec le triplet `x64-windows-static` : la DLL ne
dépend d'aucune DLL tierce.

```powershell
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

Les trois fichiers sont produits dans `build/Release/`.

Organisation du code :

| Dossier | Contenu |
|---|---|
| `injector/` | Lanceur 64 bits (CreateProcess + injection LoadLibrary) |
| `src/render/` | Hooks Direct3D 9Ex (PresentEx, cibles de rendu), partage des images vers D3D11, fenêtre du jeu |
| `src/xr/` | Session OpenXR, manettes, rendu des manettes, clavier virtuel, menu radial |
| `src/engine/` | Moteur du jeu : caméra stéréo, squelette (bras VR), combat, ordres, saisie de texte, son, cadence |
| `src/input/` | Injection DirectInput (clavier, souris), pointeur des menus |
| `src/util/` | Journal, recherche de signatures, réglages |
| `config_tool/` | Outil de configuration (Win32) |
| `tools/` | Scripts d'analyse (signatures, références) |

## Licence et crédits

WarbandVR est distribué sous [licence MIT](LICENSE).

- Bibliothèques tierces liées dans les binaires : **MinHook** (BSD 2-Clause), **chargeur du
  SDK OpenXR** (Apache 2.0), **JsonCpp** (MIT) : voir `THIRD_PARTY_NOTICES.txt` dans la
  release.
- Architecture inspirée de **WSE** (Warband Script Enhancer, WTFPL) ; WarbandVR fonctionne
  par-dessus **WSE2**, qu'il ne modifie pas.
- Mount & Blade: Warband est un jeu de TaleWorlds Entertainment. Ce projet n'est ni affilié
  à TaleWorlds ou aux auteurs de WSE2, ni approuvé par eux.
