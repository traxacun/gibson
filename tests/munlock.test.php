<?php 

require_once 'testlib.php';

$g = new Gibson();

fail_if( $g->pconnect(GIBSON_SOCKET) == FALSE, "Could not connect to test instance" );
fail_if( $g->set( "foo",  "bar" )      == FALSE, "Unexpected SET reply" );
fail_if( $g->set( "fuu",  "bur" )      == FALSE, "Unexpected SET reply" );
fail_if( $g->mlock( "f", 1 )           == FALSE, "Unexpected MLOCK reply" );

fail_if( $g->set( "foo", "new" ) != FALSE, "Value should be locked" );
fail_if( $g->set( "fuu", "new" ) != FALSE, "Value should be locked" );

fail_if( $g->munlock( "f" )           == FALSE, "Unexpected MUNLOCK reply" );

fail_if( $g->set( "foo", "new" ) == FALSE, "Value should be unlocked" );
fail_if( $g->set( "fuu", "new" ) == FALSE, "Value should be unlocked" );

?>
