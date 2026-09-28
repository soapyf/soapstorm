# Community content

Community content is a shared collection of starter poses, clips and animations kept in a public git
repository under CC0, so anyone can use it for anything. You clone the repository to your computer and
add the folder to the **Inventory** with **Add Community Folder...**; VATs then lists its files beside your
own.

> Related articles: [[Project library]], [[Pose library]], [[Face tracking]], [[Anim format]]

## Usage

### Get a community folder

Clone the repository with git, or download it as a zip and unpack it, into any folder you like:

```
git clone <repository URL> vats-community
```

VATs does not download anything itself. Run `git pull` in that folder now and then for new content.

### Add it to the Inventory

1. Open the **Inventory** tab.
2. In the **Projects** section, press **Add Community Folder...** and choose the folder you cloned.

VATs adds the folder's subfolders it knows to the Inventory, the same way **Add Folder...** adds a folder:

| Subfolder | Listed under |
|---|---|
| `poses` | **Animations** |
| `clips` | **Animations** |
| `animations` | **Animations** |
| `projects` | **Projects** |

Each becomes its own group, titled with the subfolder's name. A folder with none of these subfolders is
added whole, to both sections. A folder already listed is not added twice. Removing a group works as for any
added folder: right-click its title and choose **Remove Folder from Inventory**.

The poses and clips are `.anim` files, so they are used like any animation in the Inventory: right-click one
and choose **Insert into Current Project at This Frame** (or **Insert Mirrored**), or drag it onto the view. A
one-frame pose inserts as a pose; a clip inserts its keys from the current frame on. See
[[Project library#Inserting an animation]].

### Face mappings

The repository's `face-mapping` folder holds one `.json` file per mesh head, describing how face-capture
shapes map onto that head's face bones. VATs does not read these files yet; they are kept in the repository
so the mappings are collected in one place.

## Contributing

Contributions go to the repository as pull requests. The rule is short:

- **CC0 only.** By contributing you dedicate the files to the public domain under
  [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/).
- **Never derived from bought content.** Nothing made from, retargeted from or edited from animations,
  poses, motion capture or mesh-head files you bought or were given under any other licence, and nothing taken
  from a viewer's cache. Content you made from scratch, and motion capture you recorded yourself, are fine.

Export a pose or clip to `.anim` with **Export SL .anim** (see [[Export to Second Life]]) and put it in the
matching folder: `poses` for one frame, `clips` for a short gesture, `animations` for a whole animation.

## Troubleshooting

### Add Community Folder... listed nothing

The groups are there but empty: the folder's `.anim` and `.vat` files are in deeper folders. Only files
directly in a listed folder are shown, as with **Add Folder...**.

## See also

- [[Project library]]
- [[Pose library]]

Category: Animating
