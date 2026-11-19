toUtf8 = function( o ) { return new TextEncoder().encode( JSON.stringify( o ) ); }
fromUtf8 = function( t ) { return JSON.parse( new TextDecoder().decode( t ) ); }
