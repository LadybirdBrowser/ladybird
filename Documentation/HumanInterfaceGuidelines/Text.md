# Guidelines for user interface text in Ladybird

## Capitalization

Ladybird employs two capitalization styles:

- Book title capitalization
- Sentence-style capitalization

### Book title capitalization

In this style, we capitalize the first letter of the first and last word,
as well as all words in between, *except* articles (a, an, the);
the seven coordinating conjunctions (for, and, nor, but, or, yet, so);
and prepositions with up to four letters (at, by, for, with, into, ...)

#### Examples:
- Create New Layer
- Copy URL
- Move to Front
- Save and Exit
- Sort by Name

#### Used for:

- Button text
- Icon labels
- Menu names
- Menu items
- Tab titles
- Window titles
- Tooltips

### Sentence-style capitalization

This style follows basic English sentence capitalization.
We capitalize the first letter of the first word, along with the first letter
of proper names, weekdays, etc.

#### Examples:
- An error occurred
- Use system defaults
- Copy the selected text
- Enable Linux compatibility hacks

#### Used for:

- Check box labels
- Group box labels
- List items
- Messages (as in message boxes)
- Radio button labels
- Status bar text
- Text box labels

## Ellipses

The ellipsis, represented by a series of three periods (...), has two special
functions in the interface:

- Eliding text
- Foreshadowing additional user input

The first occurs programmatically, but the second requires care when setting
text manually.

Control text which implies an action whose effect is incomplete pending further
user input should end in an ellipsis. Opening a new window does not in itself
justify the use of an ellipsis; the dialog must be an intermediate step toward
completing the action.

Ellipses should be used sparingly elsewhere to avoid confusion with elision.

#### Examples:
- Save As...
- Browse...
- Insert Emoji...
