package tree_sitter_okf_test

import (
	"testing"

	tree_sitter "github.com/tree-sitter/go-tree-sitter"
	tree_sitter_okf "github.com/antstanley/tree-sitter-okf/bindings/go"
)

func TestCanLoadGrammar(t *testing.T) {
	language := tree_sitter.NewLanguage(tree_sitter_okf.Language())
	if language == nil {
		t.Errorf("Error loading OKF grammar")
	}
}
