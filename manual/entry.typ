// One chapter, by name: typst compile --input chapter=01-overview entry.typ
#import "/template.typ": *
#import "/chapters.typ": chapters
#let slug = sys.inputs.chapter
#let info = chapters.find(c => c.file == slug)
#chapter-page(slug, info.title)[
  #include "/chapters/" + slug + ".typ"
]
